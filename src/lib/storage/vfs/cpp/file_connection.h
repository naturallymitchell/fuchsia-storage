// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_VFS_CPP_FILE_CONNECTION_H_
#define SRC_LIB_STORAGE_VFS_CPP_FILE_CONNECTION_H_

#ifndef __Fuchsia__
#error "Fuchsia-only header"
#endif

#include <fidl/fuchsia.io/cpp/wire.h>

#include "src/lib/storage/vfs/cpp/connection.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace fs {

namespace internal {

class FileConnection : public Connection, public fidl::WireServer<fuchsia_io::File> {
 public:
  // Refer to documentation for |Connection::Connection|.
  FileConnection(fs::FuchsiaVfs* vfs, fbl::RefPtr<fs::Vnode> vnode, VnodeProtocol protocol,
                 VnodeConnectionOptions options);

  ~FileConnection() = default;

 protected:
  void OnTeardown();

  //
  // |fuchsia.io/Node| operations.
  //

  void Clone(CloneRequestView request, CloneCompleter::Sync& completer) final;
  void Close(CloseRequestView request, CloseCompleter::Sync& completer) final;
  void Close2(Close2RequestView request, Close2Completer::Sync& completer) final;
  void Describe(DescribeRequestView request, DescribeCompleter::Sync& completer) final;
  void Describe2(Describe2RequestView request, Describe2Completer::Sync& completer) final;
  void Sync(SyncRequestView request, SyncCompleter::Sync& completer) final;
  void Sync2(Sync2RequestView request, Sync2Completer::Sync& completer) final;
  void GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) final;
  void SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) final;
  void GetFlags(GetFlagsRequestView request, GetFlagsCompleter::Sync& completer) final;
  void SetFlags(SetFlagsRequestView request, SetFlagsCompleter::Sync& completer) final;

  //
  // |fuchsia.io/File| operations.
  //

  void Truncate(TruncateRequestView request, TruncateCompleter::Sync& completer) final;
  void Resize(ResizeRequestView request, ResizeCompleter::Sync& completer) final;
  void GetFlagsDeprecatedUseNode(GetFlagsDeprecatedUseNodeRequestView request,
                                 GetFlagsDeprecatedUseNodeCompleter::Sync& completer) final;
  void SetFlagsDeprecatedUseNode(SetFlagsDeprecatedUseNodeRequestView request,
                                 SetFlagsDeprecatedUseNodeCompleter::Sync& completer) final;
  void GetBuffer(GetBufferRequestView request, GetBufferCompleter::Sync& completer) final;
  void GetBackingMemory(GetBackingMemoryRequestView request,
                        GetBackingMemoryCompleter::Sync& completer) final;

  //
  // |fuchsia.io/AdvisoryLocking| operations.
  //

  void AdvisoryLock(fidl::WireServer<fuchsia_io::File>::AdvisoryLockRequestView request,
                    AdvisoryLockCompleter::Sync& _completer) final;

 private:
  zx_status_t ResizeInternal(uint64_t length);
  zx_status_t GetBackingMemoryInternal(fuchsia_io::wire::VmoFlags flags, zx::vmo* out_vmo,
                                       size_t* out_size);
};

}  // namespace internal

}  // namespace fs

#endif  // SRC_LIB_STORAGE_VFS_CPP_FILE_CONNECTION_H_
