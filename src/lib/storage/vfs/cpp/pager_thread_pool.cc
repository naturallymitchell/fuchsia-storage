// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/pager_thread_pool.h"

#include <zircon/assert.h>
#include <zircon/syscalls/port.h>
#include <zircon/syscalls/types.h>

#include <fbl/auto_lock.h>

#include "src/lib/storage/vfs/cpp/paged_vfs.h"

namespace fs {

PagerThreadPool::PagerThreadPool(PagedVfs& vfs, int num_threads)
    : vfs_(vfs), num_threads_(num_threads) {}

PagerThreadPool::~PagerThreadPool() {
  // The loop will treat a USER packet as the quit event so we can synchronize with it.
  zx_port_packet_t quit_packet{};
  quit_packet.type = ZX_PKT_TYPE_USER;

  // Each thread will quit as soon as it reads one quit packet, so post that many packets.
  for (int i = 0; i < num_threads_; i++)
    port_.queue(&quit_packet);

  for (auto& thread : threads_) {
    thread->join();
    thread.reset();
  }
  threads_.clear();
}

zx::status<> PagerThreadPool::Init() {
  if (zx_status_t status = zx::port::create(0, &port_); status != ZX_OK)
    return zx::error(status);

  // Start all the threads.
  for (int i = 0; i < num_threads_; i++)
    threads_.push_back(std::make_unique<std::thread>([self = this]() { self->ThreadProc(); }));

  return zx::ok();
}

void PagerThreadPool::ThreadProc() {
  while (true) {
    zx_port_packet_t packet;
    if (zx_status_t status = port_.wait(zx::time::infinite(), &packet); status != ZX_OK) {
      // TODO(brettw) it would be nice to log from here but some drivers that depend on this
      // library aren't allowed to log.
      // FX_LOGST(ERROR, "pager") << "Pager port wait failed, stopping. The system will probably go
      // down.";
      return;
    }

    if (packet.type == ZX_PKT_TYPE_USER)
      break;  // USER packets tell us to quit.

    // Should only be getting pager requests on this port.
    ZX_ASSERT(packet.type == ZX_PKT_TYPE_PAGE_REQUEST);

    switch (packet.page_request.command) {
      case ZX_PAGER_VMO_READ:
        vfs_.PagerVmoRead(packet.key, packet.page_request.offset, packet.page_request.length);
        break;
      case ZX_PAGER_VMO_COMPLETE:
        // We don't currently do anything on "complete" requests. There are two ways that a paged
        // VMO can be torn down:
        //
        //  - The "natural" way when there are no more references to it. The PagedVnode watches for
        //    the "zero children" notification to detect this condition and clean up. In this
        //    case there is no "complete" notification from the kernel.
        //
        //  - Some code could decide to stop paging when there are still references to the VMO.
        //    In this case it will call zx_pager_detach_vmo() and the kernel will call us back with
        //    ZX_PAGER_VMO_COMPLETE to tell us that it's processed that request and will not send
        //    any more pager requests for that VMO (even if there are still references to it). We
        //    currently don't have any need for this case. And since we'll automatically fail
        //    requests for VMOs we're no longer paging, it's not clear we'll ever need to
        //    synchronize with the kernel in this manner.
        break;
      default:
        // Unexpected request.
        ZX_ASSERT(false);
        break;
    }
  }
}

}  // namespace fs
