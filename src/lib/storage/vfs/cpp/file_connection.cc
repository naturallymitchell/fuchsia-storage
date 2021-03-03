// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/file_connection.h"

#include <fcntl.h>
#include <fuchsia/io/llcpp/fidl.h>
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

#include "src/lib/storage/vfs/cpp/debug.h"
#include "src/lib/storage/vfs/cpp/fidl_transaction.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace fio = ::llcpp::fuchsia::io;

namespace fs {

namespace internal {

FileConnection::FileConnection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode, VnodeProtocol protocol,
                               VnodeConnectionOptions options)
    : Connection(vfs, std::move(vnode), protocol, options, FidlProtocol::Create<fio::File>(this)) {}

void FileConnection::Clone(uint32_t clone_flags, fidl::ServerEnd<fio::Node> object,
                           CloneCompleter::Sync& completer) {
  Connection::NodeClone(clone_flags, std::move(object));
}

void FileConnection::Close(CloseCompleter::Sync& completer) {
  auto result = Connection::NodeClose();
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void FileConnection::Describe(DescribeCompleter::Sync& completer) {
  auto result = Connection::NodeDescribe();
  if (result.is_error()) {
    return completer.Close(result.error());
  }
  ConvertToIoV1NodeInfo(result.take_value(),
                        [&](fio::wire::NodeInfo&& info) { completer.Reply(std::move(info)); });
}

void FileConnection::Sync(SyncCompleter::Sync& completer) {
  Connection::NodeSync([completer = completer.ToAsync()](zx_status_t sync_status) mutable {
    completer.Reply(sync_status);
  });
}

void FileConnection::GetAttr(GetAttrCompleter::Sync& completer) {
  auto result = Connection::NodeGetAttr();
  if (result.is_error()) {
    completer.Reply(result.error(), fio::wire::NodeAttributes());
  } else {
    completer.Reply(ZX_OK, result.value().ToIoV1NodeAttributes());
  }
}

void FileConnection::SetAttr(uint32_t flags, ::llcpp::fuchsia::io::wire::NodeAttributes attributes,
                             SetAttrCompleter::Sync& completer) {
  auto result = Connection::NodeSetAttr(flags, attributes);
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void FileConnection::NodeGetFlags(NodeGetFlagsCompleter::Sync& completer) {
  auto result = Connection::NodeNodeGetFlags();
  if (result.is_error()) {
    completer.Reply(result.error(), 0);
  } else {
    completer.Reply(ZX_OK, result.value());
  }
}

void FileConnection::NodeSetFlags(uint32_t flags, NodeSetFlagsCompleter::Sync& completer) {
  auto result = Connection::NodeNodeSetFlags(flags);
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void FileConnection::Truncate(uint64_t length, TruncateCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[FileTruncate] options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }

  zx_status_t status = vnode()->Truncate(length);
  completer.Reply(status);
}

void FileConnection::GetFlags(GetFlagsCompleter::Sync& completer) {
  uint32_t flags = options().ToIoV1Flags() & (kStatusFlags | ZX_FS_RIGHTS);
  completer.Reply(ZX_OK, flags);
}

void FileConnection::SetFlags(uint32_t flags, SetFlagsCompleter::Sync& completer) {
  auto options = VnodeConnectionOptions::FromIoV1Flags(flags);
  set_append(options.flags.append);
  completer.Reply(ZX_OK);
}

void FileConnection::GetBuffer(uint32_t flags, GetBufferCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[FileGetBuffer] our options: ", options(),
                        ", incoming flags: ", ZxFlags(flags));

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE, nullptr);
  } else if ((flags & fio::wire::VMO_FLAG_PRIVATE) && (flags & fio::wire::VMO_FLAG_EXACT)) {
    completer.Reply(ZX_ERR_INVALID_ARGS, nullptr);
  } else if ((options().flags.append) && (flags & fio::wire::VMO_FLAG_WRITE)) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, nullptr);
  } else if (!options().rights.write && (flags & fio::wire::VMO_FLAG_WRITE)) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, nullptr);
  } else if (!options().rights.execute && (flags & fio::wire::VMO_FLAG_EXEC)) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, nullptr);
  } else if (!options().rights.read) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, nullptr);
  } else {
    ::llcpp::fuchsia::mem::wire::Buffer buffer;
    zx_status_t status = vnode()->GetVmo(flags, &buffer.vmo, &buffer.size);
    completer.Reply(status, status == ZX_OK ? fidl::unowned_ptr(&buffer) : nullptr);
  }
}

}  // namespace internal

}  // namespace fs
