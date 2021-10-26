// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async-testing/test_loop.h>

#include <zxtest/zxtest.h>

#include "src/lib/storage/vfs/cpp/managed_vfs.h"
#include "src/lib/storage/vfs/cpp/pseudo_dir.h"
#include "src/lib/storage/vfs/cpp/synchronous_vfs.h"

namespace {

// Simple vnode implementation that provides a way to query whether the vfs pointer is set.
class TestNode : public fs::Vnode {
 public:
  // Vnode implementation:
  fs::VnodeProtocolSet GetProtocols() const override { return fs::VnodeProtocol::kFile; }
  zx_status_t GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights,
                                     fs::VnodeRepresentation* info) final {
    if (protocol == fs::VnodeProtocol::kFile) {
      *info = fs::VnodeRepresentation::File();
      return ZX_OK;
    }
    return ZX_ERR_NOT_SUPPORTED;
  }

  bool HasVfsPointer() {
    std::lock_guard lock(mutex_);
    return !!vfs();
  }

 private:
  friend fbl::internal::MakeRefCountedHelper<TestNode>;
  friend fbl::RefPtr<TestNode>;

  explicit TestNode(fs::FuchsiaVfs* vfs) : Vnode(vfs) {}
  ~TestNode() override {}
};

}  // namespace

// ManagedVfs always sets the dispatcher in its constructor, and trying to change it using
// Vfs::SetDispatcher should fail.
TEST(ManagedVfs, CantSetDispatcher) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  fs::ManagedVfs vfs(loop.dispatcher());
  ASSERT_DEATH([&]() { vfs.SetDispatcher(loop.dispatcher()); });
}

TEST(SynchronousVfs, CanOnlySetDispatcherOnce) {
  fs::SynchronousVfs vfs;
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  vfs.SetDispatcher(loop.dispatcher());

  ASSERT_DEATH([&]() { vfs.SetDispatcher(loop.dispatcher()); });
}

TEST(SynchronousVfs, UnmountAndShutdown) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  fs::SynchronousVfs vfs(loop.dispatcher());
  loop.StartThread();

  zx::status root = fidl::CreateEndpoints<fuchsia_io_admin::DirectoryAdmin>();
  ASSERT_EQ(ZX_OK, root.status_value());

  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();
  ASSERT_OK(vfs.ServeDirectory(std::move(dir),
                               fidl::ServerEnd<fuchsia_io::Directory>(root->server.TakeChannel())));

  auto result = fidl::WireCall(root->client)->Unmount();
  ASSERT_OK(result.status());
  ASSERT_OK(result->s);
  ASSERT_TRUE(vfs.IsTerminating());
}

TEST(ManagedVfs, UnmountAndShutdown) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  fs::ManagedVfs vfs(loop.dispatcher());
  loop.StartThread();

  zx::status root = fidl::CreateEndpoints<fuchsia_io_admin::DirectoryAdmin>();
  ASSERT_EQ(ZX_OK, root.status_value());

  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();
  ASSERT_OK(vfs.ServeDirectory(std::move(dir),
                               fidl::ServerEnd<fuchsia_io::Directory>(root->server.TakeChannel())));

  auto result = fidl::WireCall(root->client)->Unmount();
  ASSERT_OK(result.status());
  ASSERT_OK(result->s);
  ASSERT_TRUE(vfs.IsTerminating());
}

static void CheckClosesConnection(fs::FuchsiaVfs* vfs, async::TestLoop* loop) {
  zx::status a = fidl::CreateEndpoints<fuchsia_io::Directory>();
  zx::status b = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(a.status_value());
  ASSERT_OK(b.status_value());

  auto dir_a = fbl::MakeRefCounted<fs::PseudoDir>();
  auto dir_b = fbl::MakeRefCounted<fs::PseudoDir>();
  ASSERT_OK(vfs->ServeDirectory(dir_a, std::move(a->server)));
  ASSERT_OK(vfs->ServeDirectory(dir_b, std::move(b->server)));
  bool callback_called = false;
  vfs->CloseAllConnectionsForVnode(*dir_a, [&callback_called]() { callback_called = true; });
  loop->RunUntilIdle();
  zx_signals_t signals;
  ASSERT_OK(a->client.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(), &signals));
  ASSERT_TRUE(signals & ZX_CHANNEL_PEER_CLOSED);
  ASSERT_EQ(ZX_ERR_TIMED_OUT,
            b->client.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time(0), &signals));
  ASSERT_TRUE(callback_called);
}

TEST(ManagedVfs, CloseAllConnections) {
  async::TestLoop loop;
  fs::ManagedVfs vfs(loop.dispatcher());
  CheckClosesConnection(&vfs, &loop);
  loop.RunUntilIdle();
}

TEST(SynchronousVfs, CloseAllConnections) {
  async::TestLoop loop;
  fs::SynchronousVfs vfs(loop.dispatcher());
  CheckClosesConnection(&vfs, &loop);
  loop.RunUntilIdle();
}

TEST(ManagedVfs, CloseAllConnectionsForVnodeWithoutAnyConnections) {
  async::TestLoop loop;
  fs::ManagedVfs vfs(loop.dispatcher());
  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();
  bool closed = false;
  vfs.CloseAllConnectionsForVnode(*dir, [&closed]() { closed = true; });
  loop.RunUntilIdle();
  ASSERT_TRUE(closed);
}

TEST(SynchronousVfs, CloseAllConnectionsForVnodeWithoutAnyConnections) {
  async::TestLoop loop;
  fs::SynchronousVfs vfs(loop.dispatcher());
  auto dir = fbl::MakeRefCounted<fs::PseudoDir>();
  bool closed = false;
  vfs.CloseAllConnectionsForVnode(*dir, [&closed]() { closed = true; });
  loop.RunUntilIdle();
  ASSERT_TRUE(closed);
}

TEST(SynchronousVfs, DeletesNodeVfsPointers) {
  async::TestLoop loop;
  auto vfs = std::make_unique<fs::SynchronousVfs>(loop.dispatcher());

  auto file = fbl::MakeRefCounted<TestNode>(vfs.get());
  EXPECT_TRUE(file->HasVfsPointer());

  // Delete the Vfs while keeping the file alive after it.
  vfs.reset();
  EXPECT_FALSE(file->HasVfsPointer());
}
