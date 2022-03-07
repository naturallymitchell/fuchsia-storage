// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/file_connection.h"

#include <fcntl.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fdio/io.h>
#include <lib/fdio/vfs.h>
#include <lib/zircon-internal/debug.h>
#include <lib/zx/handle.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zircon/assert.h>

#include <memory>
#include <type_traits>
#include <utility>

#include <fbl/string_buffer.h>

#include "src/lib/storage/vfs/cpp/advisory_lock.h"
#include "src/lib/storage/vfs/cpp/debug.h"
#include "src/lib/storage/vfs/cpp/fidl_transaction.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace fio = fuchsia_io;

namespace fs {

namespace internal {

FileConnection::FileConnection(fs::FuchsiaVfs* vfs, fbl::RefPtr<fs::Vnode> vnode,
                               VnodeProtocol protocol, VnodeConnectionOptions options)
    : Connection(vfs, std::move(vnode), protocol, options, FidlProtocol::Create<fio::File>(this)) {}

void FileConnection::Clone(CloneRequestView request, CloneCompleter::Sync& completer) {
  Connection::NodeClone(request->flags, std::move(request->object));
}

void FileConnection::CloseDeprecated(CloseDeprecatedRequestView request,
                                     CloseDeprecatedCompleter::Sync& completer) {
  zx::status result = Connection::NodeClose();
  if (result.is_error()) {
    completer.Reply(result.status_value());
  } else {
    completer.Reply(ZX_OK);
  }
}

void FileConnection::Close(CloseRequestView request, CloseCompleter::Sync& completer) {
  zx::status result = Connection::NodeClose();
  if (result.is_error()) {
    completer.ReplyError(result.status_value());
  } else {
    completer.ReplySuccess();
  }
}

void FileConnection::Describe(DescribeRequestView request, DescribeCompleter::Sync& completer) {
  zx::status result = Connection::NodeDescribe();
  if (result.is_error()) {
    return completer.Close(result.status_value());
  }
  ConvertToIoV1NodeInfo(std::move(result).value(),
                        [&](fio::wire::NodeInfo&& info) { completer.Reply(std::move(info)); });
}

void FileConnection::Describe2(Describe2RequestView request, Describe2Completer::Sync& completer) {
  zx::status result = Connection::NodeDescribe();
  if (result.is_error()) {
    completer.Close(result.status_value());
    return;
  }
  ConnectionInfoConverter converter(std::move(result).value());
  completer.Reply(std::move(converter.info));
}

void FileConnection::SyncDeprecated(SyncDeprecatedRequestView request,
                                    SyncDeprecatedCompleter::Sync& completer) {
  Connection::NodeSync([completer = completer.ToAsync()](zx_status_t sync_status) mutable {
    completer.Reply(sync_status);
  });
}

void FileConnection::Sync(SyncRequestView request, SyncCompleter::Sync& completer) {
  Connection::NodeSync([completer = completer.ToAsync()](zx_status_t sync_status) mutable {
    if (sync_status != ZX_OK) {
      completer.ReplyError(sync_status);
    } else {
      completer.ReplySuccess();
    }
  });
}

void FileConnection::GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) {
  zx::status result = Connection::NodeGetAttr();
  if (result.is_error()) {
    completer.Reply(result.status_value(), fio::wire::NodeAttributes());
  } else {
    completer.Reply(ZX_OK, result.value().ToIoV1NodeAttributes());
  }
}

void FileConnection::SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) {
  zx::status result = Connection::NodeSetAttr(request->flags, request->attributes);
  if (result.is_error()) {
    completer.Reply(result.status_value());
  } else {
    completer.Reply(ZX_OK);
  }
}

void FileConnection::GetFlagsDeprecatedUseNode(
    GetFlagsDeprecatedUseNodeRequestView request,
    GetFlagsDeprecatedUseNodeCompleter::Sync& completer) {
  zx::status result = Connection::NodeGetFlags();
  if (result.is_error()) {
    completer.Reply(result.status_value(), 0);
  } else {
    completer.Reply(ZX_OK, result.value());
  }
}

void FileConnection::SetFlagsDeprecatedUseNode(
    SetFlagsDeprecatedUseNodeRequestView request,
    SetFlagsDeprecatedUseNodeCompleter::Sync& completer) {
  zx::status result = Connection::NodeSetFlags(request->flags);
  if (result.is_error()) {
    completer.Reply(result.status_value());
  } else {
    completer.Reply(ZX_OK);
  }
}

zx_status_t FileConnection::ResizeInternal(uint64_t length) {
  FS_PRETTY_TRACE_DEBUG("[FileTruncate] options: ", options());

  if (options().flags.node_reference) {
    return ZX_ERR_BAD_HANDLE;
  }
  if (!options().rights.write) {
    return ZX_ERR_BAD_HANDLE;
  }

  return vnode()->Truncate(length);
}

void FileConnection::TruncateDeprecatedUseResize(
    TruncateDeprecatedUseResizeRequestView request,
    TruncateDeprecatedUseResizeCompleter::Sync& completer) {
  completer.Reply(ResizeInternal(request->length));
}

void FileConnection::Resize(ResizeRequestView request, ResizeCompleter::Sync& completer) {
  zx_status_t result = ResizeInternal(request->length);
  if (result != ZX_OK) {
    completer.ReplyError(result);
  } else {
    completer.ReplySuccess();
  }
}

void FileConnection::GetFlags(GetFlagsRequestView request, GetFlagsCompleter::Sync& completer) {
  uint32_t flags = options().ToIoV1Flags() & (kStatusFlags | ZX_FS_RIGHTS);
  completer.Reply(ZX_OK, flags);
}

void FileConnection::SetFlags(SetFlagsRequestView request, SetFlagsCompleter::Sync& completer) {
  auto options = VnodeConnectionOptions::FromIoV1Flags(request->flags);
  set_append(options.flags.append);
  completer.Reply(ZX_OK);
}

zx_status_t FileConnection::GetBackingMemoryInternal(fuchsia_io::wire::VmoFlags flags,
                                                     zx::vmo* out_vmo, size_t* out_size) {
  if (options().flags.node_reference) {
    return ZX_ERR_BAD_HANDLE;
  } else if ((flags & fio::wire::VmoFlags::kPrivateClone) &&
             (flags & fio::wire::VmoFlags::kSharedBuffer)) {
    return ZX_ERR_INVALID_ARGS;
  } else if (options().flags.append && (flags & fio::wire::VmoFlags::kWrite)) {
    return ZX_ERR_ACCESS_DENIED;
  } else if (!options().rights.write && (flags & fio::wire::VmoFlags::kWrite)) {
    return ZX_ERR_ACCESS_DENIED;
  } else if (!options().rights.execute && (flags & fio::wire::VmoFlags::kExecute)) {
    return ZX_ERR_ACCESS_DENIED;
  } else if (!options().rights.read) {
    return ZX_ERR_ACCESS_DENIED;
  } else {
    return vnode()->GetVmo(flags, out_vmo, out_size);
  }
}

void FileConnection::GetBufferDeprecatedUseGetBackingMemory(
    GetBufferDeprecatedUseGetBackingMemoryRequestView request,
    GetBufferDeprecatedUseGetBackingMemoryCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[FileGetBuffer] our options: ", options(),
                        ", incoming flags: ", ZxFlags(request->flags));
  fio::wire::VmoFlags flags = fio::wire::VmoFlags::TruncatingUnknown(request->flags);

  fuchsia_mem::wire::Buffer buffer;
  zx_status_t status = GetBackingMemoryInternal(flags, &buffer.vmo, &buffer.size);
  completer.Reply(status, status == ZX_OK
                              ? fidl::ObjectView<fuchsia_mem::wire::Buffer>::FromExternal(&buffer)
                              : nullptr);
}

void FileConnection::GetBackingMemory(GetBackingMemoryRequestView request,
                                      GetBackingMemoryCompleter::Sync& completer) {
  zx::vmo vmo;
  size_t size = 0;
  zx_status_t status = GetBackingMemoryInternal(request->flags, &vmo, &size);
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    completer.ReplySuccess(std::move(vmo));
  }
}

void FileConnection::AdvisoryLock(
    fidl::WireServer<fuchsia_io::File>::AdvisoryLockRequestView request,
    AdvisoryLockCompleter::Sync& completer) {
  zx_koid_t owner = GetChannelOwnerKoid();
  // advisory_lock replies to the completer
  auto async_completer = completer.ToAsync();
  fit::callback<void(zx_status_t)> callback = file_lock::lock_completer_t(
      [lock_completer = std::move(async_completer)](zx_status_t status) mutable {
        lock_completer.ReplyError(status);
      });

  advisory_lock(owner, vnode(), true, request->request, std::move(callback));
}

void FileConnection::OnTeardown() {
  auto owner = GetChannelOwnerKoid();
  vnode()->DeleteFileLockInTeardown(owner);
}

}  // namespace internal

}  // namespace fs
