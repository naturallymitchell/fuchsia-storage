// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_VFS_CPP_PAGED_VFS_H_
#define SRC_LIB_STORAGE_VFS_CPP_PAGED_VFS_H_

#include <lib/zx/pager.h>
#include <lib/zx/status.h>
#include <lib/zx/vmo.h>

#include "src/lib/storage/vfs/cpp/managed_vfs.h"
#include "src/lib/storage/vfs/cpp/pager_thread_pool.h"

namespace fs {

class PagedVnode;

// A variant of Vfs that supports paging. A PagedVfs supports PagedVnode objects.
//
// UNDER DEVELOPMENT
// =================
// Paging in the fs library is currently under active development and not ready to use yet.
// See http://fxbug.dev/51111. Long-term the paging functionality should be moved into ManagedVfs
// and all C++ filesystems should use that on Fuchsia.
class PagedVfs : public ManagedVfs {
 public:
  // The caller must call Init() which must succeed before using this class.
  explicit PagedVfs(async_dispatcher_t* dispatcher, int num_pager_threads = 1);
  ~PagedVfs() override;

  // Creates the pager and worker threads. If any of these fail, this class should no be used.
  zx::status<> Init();

  // Called in response to a successful PagedVnode::VmoRead() request, this supplies paged data from
  // aux_vmo to the PagedVnode's VMO to the kernel. See zx_pager_supply_pages() documentation for
  // more.
  zx::status<> SupplyPages(zx::vmo& node_vmo, uint64_t offset, uint64_t length, zx::vmo& aux_vmo,
                           uint64_t aux_offset);

  // Called in response to a failed PagedVnode::VmoRead() request, this reports that there was an
  // error populating page data. See zx_pager_op_range() documentation for more, only certain
  // values are permitted for err.
  zx::status<> ReportPagerError(zx::vmo& node_vmo, uint64_t offset, uint64_t length,
                                zx_status_t err);

  // Allocates a VMO of the given size associated with the given PagedVnode. VMOs for use with
  // the pager must be allocated by this method so the page requests are routed to the correct
  // PagedVnode.
  //
  // This function takes a reference to the vnode on behalf of the kernel paging system. This
  // reference will be released when the PagedNode notices there are no references to the VMO.
  //
  // This function is for internal use by PagedVnode. Most callers should use
  // PagedVnode::EnsureCreateVmo().
  zx::status<zx::vmo> CreatePagedNodeVmo(fbl::RefPtr<PagedVnode> node, uint64_t size);

  // Callback that the PagerThreadPool uses to notify us of pager events. These calls will get
  // issued on arbitrary threads.
  void PagerVmoRead(uint64_t node_id, uint64_t offset, uint64_t length);

 private:
  std::unique_ptr<PagerThreadPool> pager_pool_;  // Threadsafe, does not need locking.
  zx::pager pager_;

  // Vnodes with active references from the kernel paging system. The owning reference here
  // represents the reference from the kernel to this paged VMO and should only be dropped when
  // the kernel is no longer paging this node.
  uint64_t next_node_id_ = 1;
  std::map<uint64_t, fbl::RefPtr<PagedVnode>> paged_nodes_ FS_TA_GUARDED(vfs_lock_);
};

}  // namespace fs

#endif  // SRC_LIB_STORAGE_VFS_CPP_PAGED_VFS_H_
