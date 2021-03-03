// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>

#include <utility>

#include <zxtest/zxtest.h>

#include "src/lib/storage/vfs/cpp/pseudo_dir.h"
#include "src/lib/storage/vfs/cpp/service.h"
#include "src/lib/storage/vfs/cpp/synchronous_vfs.h"

namespace {

namespace fio = ::llcpp::fuchsia::io;

TEST(Service, ConstructWithRawChannelConnector) {
  auto svc = fbl::MakeRefCounted<fs::Service>([](zx::channel channel) { return ZX_OK; });
}

TEST(Service, ConstructWithTypedChannelConnector) {
  auto svc = fbl::MakeRefCounted<fs::Service>(
      [](fidl::ServerEnd<fio::Directory> server_end) { return ZX_OK; });
}

TEST(Service, ApiTest) {
  // Set up a service which can only be bound once (to make it easy to simulate an error to test
  // error reporting behavior from the connector)
  zx::channel bound_channel;
  auto svc = fbl::MakeRefCounted<fs::Service>([&bound_channel](zx::channel channel) {
    if (bound_channel)
      return ZX_ERR_IO;
    bound_channel = std::move(channel);
    return ZX_OK;
  });

  fs::VnodeConnectionOptions options_readable;
  options_readable.rights.read = true;

  // open
  fbl::RefPtr<fs::Vnode> redirect;
  auto result = svc->ValidateOptions(options_readable);
  EXPECT_TRUE(result.is_ok());
  EXPECT_EQ(ZX_OK, svc->Open(result.value(), &redirect));
  EXPECT_NULL(redirect);

  // get attr
  fs::VnodeAttributes attr;
  EXPECT_EQ(ZX_OK, svc->GetAttributes(&attr));
  EXPECT_EQ(V_TYPE_FILE, attr.mode);
  EXPECT_EQ(1, attr.link_count);

  // make some channels we can use for testing
  zx::channel c1, c2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0u, &c1, &c2));
  zx_handle_t hc1 = c1.get();

  // serve, the connector will return success the first time
  fs::SynchronousVfs vfs;
  EXPECT_EQ(ZX_OK, vfs.Serve(svc, std::move(c1), options_readable));
  EXPECT_EQ(hc1, bound_channel.get());

  // The connector will return failure because bound_channel is still valid we test that the error
  // is propagated back up through Serve,
  EXPECT_EQ(ZX_ERR_IO, vfs.Serve(svc, std::move(c2), options_readable));
  EXPECT_EQ(hc1, bound_channel.get());
}

TEST(Service, ServeDirectory) {
  zx::channel client, server;
  EXPECT_EQ(ZX_OK, zx::channel::create(0u, &client, &server));

  // open client
  zx::channel c1, c2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0u, &c1, &c2));
  EXPECT_EQ(ZX_OK, fdio_service_connect_at(client.get(), "abc", c2.release()));

  // Close client. We test the semantic that a pending open is processed even if the client has been
  // closed.
  client.reset();

  // serve
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  fs::SynchronousVfs vfs(loop.dispatcher());

  auto directory = fbl::MakeRefCounted<fs::PseudoDir>();
  auto vnode = fbl::MakeRefCounted<fs::Service>([&loop](zx::channel channel) {
    loop.Shutdown();
    return ZX_OK;
  });
  directory->AddEntry("abc", vnode);

  EXPECT_EQ(ZX_OK, vfs.ServeDirectory(directory, std::move(server)));
  EXPECT_EQ(ZX_ERR_BAD_STATE, loop.RunUntilIdle());
}

TEST(Service, ServiceNodeIsNotDirectory) {
  // Set up the server
  zx::channel client_end, server_end;
  ASSERT_EQ(ZX_OK, zx::channel::create(0u, &client_end, &server_end));

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  fs::SynchronousVfs vfs(loop.dispatcher());

  auto directory = fbl::MakeRefCounted<fs::PseudoDir>();
  auto vnode = fbl::MakeRefCounted<fs::Service>([](zx::channel channel) {
    // Should never reach here, because the directory flag is not allowed.
    EXPECT_TRUE(false, "Should not be able to open the service");
    channel.reset();
    return ZX_OK;
  });
  directory->AddEntry("abc", vnode);
  ASSERT_EQ(ZX_OK, vfs.ServeDirectory(directory, std::move(server_end)));

  // Call |ValidateOptions| with the directory flag should fail.
  auto result = vnode->ValidateOptions(fs::VnodeConnectionOptions::ReadWrite().set_directory());
  ASSERT_TRUE(result.is_error());
  ASSERT_EQ(ZX_ERR_NOT_DIR, result.error());

  // Open the service through FIDL with the directory flag, which should fail.
  zx::channel abc_client_end, abc_server_end;
  ASSERT_EQ(ZX_OK, zx::channel::create(0u, &abc_client_end, &abc_server_end));

  loop.StartThread();

  auto open_result = fio::Directory::Call::Open(
      zx::unowned_channel(client_end),
      fio::wire::OPEN_FLAG_DESCRIBE | fio::wire::OPEN_FLAG_DIRECTORY |
          fio::wire::OPEN_RIGHT_READABLE | fio::wire::OPEN_RIGHT_WRITABLE,
      0755, fidl::StringView("abc"), std::move(abc_server_end));
  EXPECT_EQ(open_result.status(), ZX_OK);
  class EventHandler : public fio::Node::SyncEventHandler {
   public:
    EventHandler() = default;

    void OnOpen(fio::Node::OnOpenResponse* event) override {
      EXPECT_EQ(ZX_ERR_NOT_DIR, event->s);
      EXPECT_TRUE(event->info.has_invalid_tag());
    }

    zx_status_t Unknown() override { return ZX_ERR_INVALID_ARGS; }
  };

  EventHandler event_handler;
  fidl::Result handler_result = event_handler.HandleOneEvent(zx::unowned_channel(abc_client_end));
  // Expect that |on_open| was received
  EXPECT_TRUE(handler_result.ok());

  loop.Shutdown();
}

TEST(Service, OpeningServiceWithNodeReferenceFlag) {
  // Set up the server
  zx::channel client_end, server_end;
  ASSERT_EQ(ZX_OK, zx::channel::create(0u, &client_end, &server_end));

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  fs::SynchronousVfs vfs(loop.dispatcher());

  auto directory = fbl::MakeRefCounted<fs::PseudoDir>();
  auto vnode = fbl::MakeRefCounted<fs::Service>([](zx::channel channel) {
    channel.reset();
    return ZX_OK;
  });
  directory->AddEntry("abc", vnode);
  ASSERT_EQ(ZX_OK, vfs.ServeDirectory(directory, std::move(server_end)));

  zx::channel abc_client_end, abc_server_end;
  ASSERT_EQ(ZX_OK, zx::channel::create(0u, &abc_client_end, &abc_server_end));

  loop.StartThread();

  auto open_result = fio::Directory::Call::Open(zx::unowned_channel(client_end),
                                                fio::wire::OPEN_FLAG_NODE_REFERENCE, 0755,
                                                fidl::StringView("abc"), std::move(abc_server_end));
  EXPECT_EQ(open_result.status(), ZX_OK);

  // The channel should speak |fuchsia.io/Node| instead of the custom service FIDL protocol. We
  // verify it by calling describe on it, which should return correctly.
  auto describe_result = fio::Node::Call::Describe(zx::unowned_channel(abc_client_end));
  ASSERT_EQ(ZX_OK, describe_result.status());
  ASSERT_EQ(fio::wire::NodeInfo::Tag::kService, describe_result->info.which());

  loop.Shutdown();
}

}  // namespace
