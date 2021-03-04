// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_VFS_CPP_NODE_CONNECTION_H_
#define SRC_LIB_STORAGE_VFS_CPP_NODE_CONNECTION_H_

#ifndef __Fuchsia__
#error "Fuchsia-only header"
#endif

#include <fuchsia/io/llcpp/fidl.h>

#include "src/lib/storage/vfs/cpp/connection.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace fs {

namespace internal {

class NodeConnection final : public Connection, public fuchsia_io::Node::Interface {
 public:
  // Refer to documentation for |Connection::Connection|.
  NodeConnection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode, VnodeProtocol protocol,
                 VnodeConnectionOptions options);

  ~NodeConnection() final = default;

 private:
  //
  // |fuchsia.io/Node| operations.
  //

  void Clone(uint32_t flags, fidl::ServerEnd<fuchsia_io::Node> object,
             CloneCompleter::Sync& completer) final;
  void Close(CloseCompleter::Sync& completer) final;
  void Describe(DescribeCompleter::Sync& completer) final;
  void Sync(SyncCompleter::Sync& completer) final;
  void GetAttr(GetAttrCompleter::Sync& completer) final;
  void SetAttr(uint32_t flags, fuchsia_io::wire::NodeAttributes attributes,
               SetAttrCompleter::Sync& completer) final;
  void NodeGetFlags(NodeGetFlagsCompleter::Sync& completer) final;
  void NodeSetFlags(uint32_t flags, NodeSetFlagsCompleter::Sync& completer) final;
};

}  // namespace internal

}  // namespace fs

#endif  // SRC_LIB_STORAGE_VFS_CPP_NODE_CONNECTION_H_