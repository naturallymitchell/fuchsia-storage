// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/nand/c/fidl.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <zircon/types.h>

#include <utility>

#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramnand.h>

namespace {

// Waits for |file| to appear in |dir|, and opens it when it does.  Times out if
// the deadline passes.
zx_status_t WaitForFile(const fbl::unique_fd& dir, const char* file, fbl::unique_fd* out) {
  auto watch_func = [](int dirfd, int event, const char* fn, void* cookie) -> zx_status_t {
    auto file = reinterpret_cast<const char*>(cookie);
    if (event != WATCH_EVENT_ADD_FILE) {
      return ZX_OK;
    }
    if (!strcmp(fn, file)) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };

  zx_status_t status =
      fdio_watch_directory(dir.get(), watch_func, ZX_TIME_INFINITE, const_cast<char*>(file));
  if (status != ZX_ERR_STOP) {
    return status;
  }
  out->reset(openat(dir.get(), file, O_RDWR));
  if (!out->is_valid()) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

}  // namespace

namespace ramdevice_client {

__EXPORT
zx_status_t RamNand::Create(const fuchsia_hardware_nand_RamNandInfo* config,
                            std::optional<RamNand>* out) {
  fbl::unique_fd control(open(kBasePath, O_RDWR));

  zx::channel ctl_svc;
  zx_status_t st = fdio_get_service_handle(control.release(), ctl_svc.reset_and_get_address());
  if (st != ZX_OK) {
    return st;
  }

  char name[fuchsia_hardware_nand_NAME_LEN + 1];
  size_t out_name_size;
  zx_status_t status;
  st = fuchsia_hardware_nand_RamNandCtlCreateDevice(ctl_svc.get(), config, &status, name,
                                                    fuchsia_hardware_nand_NAME_LEN, &out_name_size);
  if (st != ZX_OK || status != ZX_OK) {
    st = st != ZX_OK ? st : status;
    fprintf(stderr, "Could not create ram_nand device, %d\n", st);
    return st;
  }
  name[out_name_size] = '\0';
  fbl::StringBuffer<PATH_MAX> path;
  path.Append(kBasePath);
  path.Append("/");
  path.Append(name);

  fbl::unique_fd ram_nand_ctl(open(kBasePath, O_RDONLY | O_DIRECTORY));
  if (!ram_nand_ctl) {
    fprintf(stderr, "Could not open ram_nand_ctl");
    return ZX_ERR_INTERNAL;
  }

  fbl::unique_fd ram_nand;
  st = WaitForFile(ram_nand_ctl, name, &ram_nand);
  if (st != ZX_OK) {
    fprintf(stderr, "Could not open ram_nand\n");
    return st;
  }

  *out = RamNand(std::move(ram_nand), path.ToString(), fbl::String(name));
  return ZX_OK;
}

__EXPORT
RamNand::~RamNand() {
  if (unbind && fd_) {
    fdio_cpp::FdioCaller caller(std::move(fd_));
    auto resp = fidl::WireCall(caller.borrow_as<fuchsia_device::Controller>())->ScheduleUnbind();
    zx_status_t status = resp.status();
    if (status == ZX_OK && resp->result.is_err()) {
      status = resp->result.err();
    }
    if (status != ZX_OK) {
      fprintf(stderr, "Could not unbind ram_nand, %d\n", status);
    }
  }
}

}  // namespace ramdevice_client
