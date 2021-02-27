// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <fuchsia/io2/llcpp/fidl.h>
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
#include "src/lib/storage/vfs/cpp/directory_connection.h"
#include "src/lib/storage/vfs/cpp/fidl_transaction.h"
#include "src/lib/storage/vfs/cpp/mount_channel.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace fio = ::llcpp::fuchsia::io;

namespace fs {

namespace {

// Performs a path walk and opens a connection to another node.
void OpenAt(Vfs* vfs, const fbl::RefPtr<Vnode>& parent, fidl::ServerEnd<fio::Node> channel,
            fbl::StringPiece path, VnodeConnectionOptions options, Rights parent_rights,
            uint32_t mode) {
  bool describe = options.flags.describe;
  vfs->Open(parent, path, options, parent_rights, mode).visit([&](auto&& result) {
    using ResultT = std::decay_t<decltype(result)>;
    using OpenResult = fs::Vfs::OpenResult;
    if constexpr (std::is_same_v<ResultT, OpenResult::Error>) {
      if (describe) {
        fio::Node::EventSender(std::move(channel)).OnOpen(result, fio::NodeInfo());
      }
    } else if constexpr (std::is_same_v<ResultT, OpenResult::Remote>) {
      // Remote handoff to a remote filesystem node.
      vfs->ForwardOpenRemote(std::move(result.vnode), std::move(channel), result.path, options,
                             mode);
    } else if constexpr (std::is_same_v<ResultT, OpenResult::RemoteRoot>) {
      // Remote handoff to a remote filesystem node.
      vfs->ForwardOpenRemote(std::move(result.vnode), std::move(channel), ".", options, mode);
    } else if constexpr (std::is_same_v<ResultT, OpenResult::Ok>) {
      // |Vfs::Open| already performs option validation for us.
      vfs->Serve(result.vnode, std::move(channel), result.validated_options);
    }
  });
}

}  // namespace

namespace internal {

DirectoryConnection::DirectoryConnection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode,
                                         VnodeProtocol protocol, VnodeConnectionOptions options)
    : Connection(vfs, std::move(vnode), protocol, options,
                 FidlProtocol::Create<fio::DirectoryAdmin>(this)) {}

void DirectoryConnection::Clone(uint32_t clone_flags, fidl::ServerEnd<fio::Node> object,
                                CloneCompleter::Sync& completer) {
  Connection::NodeClone(clone_flags, std::move(object));
}

void DirectoryConnection::Close(CloseCompleter::Sync& completer) {
  auto result = Connection::NodeClose();
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void DirectoryConnection::Describe(DescribeCompleter::Sync& completer) {
  auto result = Connection::NodeDescribe();
  if (result.is_error()) {
    completer.Close(result.error());
    return;
  }
  ConvertToIoV1NodeInfo(result.take_value(),
                        [&](fio::NodeInfo&& info) { completer.Reply(std::move(info)); });
}

void DirectoryConnection::Sync(SyncCompleter::Sync& completer) {
  Connection::NodeSync([completer = completer.ToAsync()](zx_status_t sync_status) mutable {
    completer.Reply(sync_status);
  });
}

void DirectoryConnection::GetAttr(GetAttrCompleter::Sync& completer) {
  auto result = Connection::NodeGetAttr();
  if (result.is_error()) {
    completer.Reply(result.error(), fio::NodeAttributes());
  } else {
    completer.Reply(ZX_OK, result.value().ToIoV1NodeAttributes());
  }
}

void DirectoryConnection::SetAttr(uint32_t flags, ::llcpp::fuchsia::io::NodeAttributes attributes,
                                  SetAttrCompleter::Sync& completer) {
  auto result = Connection::NodeSetAttr(flags, attributes);
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void DirectoryConnection::NodeGetFlags(NodeGetFlagsCompleter::Sync& completer) {
  auto result = Connection::NodeNodeGetFlags();
  if (result.is_error()) {
    completer.Reply(result.error(), 0);
  } else {
    completer.Reply(ZX_OK, result.value());
  }
}

void DirectoryConnection::NodeSetFlags(uint32_t flags, NodeSetFlagsCompleter::Sync& completer) {
  auto result = Connection::NodeNodeSetFlags(flags);
  if (result.is_error()) {
    completer.Reply(result.error());
  } else {
    completer.Reply(ZX_OK);
  }
}

void DirectoryConnection::Open(uint32_t open_flags, uint32_t mode, fidl::StringView path,
                               fidl::ServerEnd<fio::Node> channel, OpenCompleter::Sync& completer) {
  auto open_options = VnodeConnectionOptions::FromIoV1Flags(open_flags);
  auto write_error = [describe = open_options.flags.describe](fidl::ServerEnd<fio::Node> channel,
                                                              zx_status_t error) {
    if (describe) {
      fio::Node::EventSender(std::move(channel)).OnOpen(error, fio::NodeInfo());
    }
  };

  if (!PrevalidateFlags(open_flags)) {
    FS_PRETTY_TRACE_DEBUG("[DirectoryOpen] prevalidate failed",
                          ", incoming flags: ", ZxFlags(open_flags), ", path: ", path.data());
    if (open_options.flags.describe) {
      return write_error(std::move(channel), ZX_ERR_INVALID_ARGS);
    }
  }

  FS_PRETTY_TRACE_DEBUG("[DirectoryOpen] our options: ", options(),
                        ", incoming options: ", open_options, ", path: ", path.data());
  if (options().flags.node_reference) {
    return write_error(std::move(channel), ZX_ERR_BAD_HANDLE);
  }
  if (open_options.flags.clone_same_rights) {
    return write_error(std::move(channel), ZX_ERR_INVALID_ARGS);
  }
  if (!open_options.flags.node_reference && !open_options.rights.any()) {
    return write_error(std::move(channel), ZX_ERR_INVALID_ARGS);
  }
  if (path.empty() || (path.size() > PATH_MAX)) {
    return write_error(std::move(channel), ZX_ERR_BAD_PATH);
  }

  // Check for directory rights inheritance
  zx_status_t status = EnforceHierarchicalRights(options().rights, open_options, &open_options);
  if (status != ZX_OK) {
    FS_PRETTY_TRACE_DEBUG("Rights violation during DirectoryOpen");
    return write_error(std::move(channel), status);
  }
  OpenAt(vfs(), vnode(), std::move(channel), fbl::StringPiece(path.data(), path.size()),
         open_options, options().rights, mode);
}

void DirectoryConnection::Unlink(fidl::StringView path, UnlinkCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryUnlink] our options: ", options(), ", path: ", path.data());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status = vfs()->Unlink(vnode(), fbl::StringPiece(path.data(), path.size()));
  completer.Reply(status);
}

void DirectoryConnection::ReadDirents(uint64_t max_out, ReadDirentsCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryReadDirents] our options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE, fidl::VectorView<uint8_t>());
    return;
  }
  if (max_out > fio::MAX_BUF) {
    completer.Reply(ZX_ERR_BAD_HANDLE, fidl::VectorView<uint8_t>());
    return;
  }
  uint8_t data[max_out];
  size_t actual = 0;
  zx_status_t status = vfs()->Readdir(vnode().get(), &dircookie_, data, max_out, &actual);
  completer.Reply(status, fidl::VectorView(fidl::unowned_ptr(data), actual));
}

void DirectoryConnection::Rewind(RewindCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryRewind] our options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  dircookie_ = VdirCookie();
  completer.Reply(ZX_OK);
}

void DirectoryConnection::GetToken(GetTokenCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryGetToken] our options: ", options());

  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE, zx::handle());
    return;
  }
  zx::event returned_token;
  zx_status_t status = vfs()->VnodeToToken(vnode(), &token(), &returned_token);
  completer.Reply(status, std::move(returned_token));
}

void DirectoryConnection::Rename(fidl::StringView src, zx::handle dst_parent_token,
                                 fidl::StringView dst, RenameCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryRename] our options: ", options(), ", src: ", src.data(),
                        ", dst: ", dst.data());

  // |fuchsia.io/Directory.Rename| only specified the token to be a generic handle; casting it here.
  zx::event token(dst_parent_token.release());

  if (src.empty() || dst.empty()) {
    completer.Reply(ZX_ERR_INVALID_ARGS);
    return;
  }
  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status =
      vfs()->Rename(std::move(token), vnode(), fbl::StringPiece(src.data(), src.size()),
                    fbl::StringPiece(dst.data(), dst.size()));
  completer.Reply(status);
}

void DirectoryConnection::Link(fidl::StringView src, zx::handle dst_parent_token,
                               fidl::StringView dst, LinkCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryLink] our options: ", options(), ", src: ", src.data(),
                        ", dst: ", dst.data());

  // |fuchsia.io/Directory.Rename| only specified the token to be a generic handle; casting it here.
  zx::event token(dst_parent_token.release());

  if (src.empty() || dst.empty()) {
    completer.Reply(ZX_ERR_INVALID_ARGS);
    return;
  }
  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  if (!options().rights.write) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status =
      vfs()->Link(std::move(token), vnode(), fbl::StringPiece(src.data(), src.size()),
                  fbl::StringPiece(dst.data(), dst.size()));
  completer.Reply(status);
}

void DirectoryConnection::Watch(uint32_t mask, uint32_t watch_options, zx::channel watcher,
                                WatchCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryWatch] our options: ", options());

  if (options().flags.node_reference) {
    completer.Reply(ZX_ERR_BAD_HANDLE);
    return;
  }
  zx_status_t status = vnode()->WatchDir(vfs(), mask, watch_options, std::move(watcher));
  completer.Reply(status);
}

void DirectoryConnection::Mount(fidl::ClientEnd<fio::Directory> remote,
                                MountCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminMount] our options: ", options());

  if (!options().rights.admin) {
    // Note: this is best-effort, and would fail if the remote endpoint
    // does not speak the |fuchsia.io/DirectoryAdmin| protocol.
    fidl::ClientEnd<fio::DirectoryAdmin> remote_admin(remote.TakeChannel());
    Vfs::UnmountHandle(std::move(remote_admin), zx::time::infinite());
    completer.Reply(ZX_ERR_ACCESS_DENIED);
    return;
  }
  MountChannel c = MountChannel(std::move(remote));
  zx_status_t status = vfs()->InstallRemote(vnode(), std::move(c));
  completer.Reply(status);
}

void DirectoryConnection::MountAndCreate(fidl::ClientEnd<fio::Directory> remote,
                                         fidl::StringView name, uint32_t flags,
                                         MountAndCreateCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminMountAndCreate] our options: ", options());

  if (!options().rights.admin) {
    // Note: this is best-effort, and would fail if the remote endpoint
    // does not speak the |fuchsia.io/DirectoryAdmin| protocol.
    fidl::ClientEnd<fio::DirectoryAdmin> remote_admin(remote.TakeChannel());
    Vfs::UnmountHandle(std::move(remote_admin), zx::time::infinite());
    completer.Reply(ZX_ERR_ACCESS_DENIED);
    return;
  }
  zx_status_t status = vfs()->MountMkdir(vnode(), fbl::StringPiece(name.data(), name.size()),
                                         MountChannel(std::move(remote)), flags);
  completer.Reply(status);
}

void DirectoryConnection::Unmount(UnmountCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminUnmount] our options: ", options());

  if (!options().rights.admin) {
    completer.Reply(ZX_ERR_ACCESS_DENIED);
    return;
  }
  Connection::UnmountAndShutdown(
      [completer = completer.ToAsync()](zx_status_t unmount_status) mutable {
        completer.Reply(unmount_status);
      });
}

void DirectoryConnection::UnmountNode(UnmountNodeCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminUnmountNode] our options: ", options());

  if (!options().rights.admin) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, zx::channel());
    return;
  }
  fidl::ClientEnd<llcpp::fuchsia::io::Directory> c;
  zx_status_t status = vfs()->UninstallRemote(vnode(), &c);
  completer.Reply(status, std::move(c));
}

void DirectoryConnection::QueryFilesystem(QueryFilesystemCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminQueryFilesystem] our options: ", options());

  fio::FilesystemInfo info;
  zx_status_t status = vnode()->QueryFilesystem(&info);
  completer.Reply(status, status == ZX_OK ? fidl::unowned_ptr(&info) : nullptr);
}

void DirectoryConnection::GetDevicePath(GetDevicePathCompleter::Sync& completer) {
  FS_PRETTY_TRACE_DEBUG("[DirectoryAdminGetDevicePath] our options: ", options());

  if (!options().rights.admin) {
    completer.Reply(ZX_ERR_ACCESS_DENIED, fidl::StringView());
    return;
  }

  char name[fio::MAX_PATH];
  size_t actual = 0;
  zx_status_t status = vnode()->GetDevicePath(sizeof(name), name, &actual);
  completer.Reply(status, fidl::StringView(name, actual));
}

}  // namespace internal

}  // namespace fs
