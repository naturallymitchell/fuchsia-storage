// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/block_client/cpp/remote_block_device.h"

#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl-utils/bind.h>
#include <lib/fzl/fifo.h>
#include <lib/zx/vmo.h>

#include <thread>
#include <unordered_set>

#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/mutex.h>
#include <gtest/gtest.h>
#include <storage/buffer/owned_vmoid.h>

namespace block_client {
namespace {

namespace fio = fuchsia_io;

constexpr uint16_t kGoldenVmoid = 2;

class MockBlockDevice {
 public:
  using Binder = fidl::Binder<MockBlockDevice>;
  zx_status_t Bind(async_dispatcher_t* dispatcher, zx::channel channel) {
    dispatcher_ = dispatcher;
    mock_node_ = std::make_unique<MockNode>(this);
    return fidl_bind(dispatcher_, channel.release(), FidlDispatch, this, nullptr);
  }

  zx_status_t ReadFifoRequests(block_fifo_request_t* requests, size_t* count) {
    zx_signals_t seen;
    zx_status_t status = fifo_.wait_one(ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED,
                                        zx::deadline_after(zx::sec(5)), &seen);
    if (status != ZX_OK) {
      return status;
    }
    return fifo_.read(requests, BLOCK_FIFO_MAX_DEPTH, count);
  }

  zx_status_t WriteFifoResponse(const block_fifo_response_t& response) {
    return fifo_.write_one(response);
  }

  bool FifoAttached() const { return fifo_.get().is_valid(); }

 private:
  // Manually dispatch to emulate the non-standard behavior of the block device,
  // which implements both the block device APIs, the Node API, and (optionally)
  // the FVM API.
  static zx_status_t FidlDispatch(void* context, fidl_txn_t* txn, fidl_incoming_msg_t* msg,
                                  const void*) {
    return reinterpret_cast<MockBlockDevice*>(context)->HandleMessage(txn, msg);
  }

  zx_status_t HandleMessage(fidl_txn_t* txn, fidl_incoming_msg_t* msg) {
    zx_status_t status = fuchsia_hardware_block_Block_try_dispatch(this, txn, msg, BlockOps());
    if (status != ZX_ERR_NOT_SUPPORTED) {
      return status;
    }
    auto incoming_msg = fidl::IncomingMessage::FromEncodedCMessage(msg);
    return fidl::WireTryDispatch<fio::Node>(mock_node_.get(), incoming_msg, nullptr) ==
                   fidl::DispatchResult::kFound
               ? ZX_OK
               : ZX_ERR_PEER_CLOSED;
  }

  static const fuchsia_hardware_block_Block_ops* BlockOps() {
    static const fuchsia_hardware_block_Block_ops kOps = {
        .GetInfo = Binder::BindMember<&MockBlockDevice::BlockGetInfo>,
        .GetStats = Binder::BindMember<&MockBlockDevice::BlockGetStats>,
        .GetFifo = Binder::BindMember<&MockBlockDevice::BlockGetFifo>,
        .AttachVmo = Binder::BindMember<&MockBlockDevice::BlockAttachVmo>,
        .CloseFifo = Binder::BindMember<&MockBlockDevice::BlockCloseFifo>,
        .RebindDevice = Binder::BindMember<&MockBlockDevice::BlockRebindDevice>,
    };
    return &kOps;
  }

  // This implementation of Node is decidedly non-standard and incomplete, but it is
  // sufficient to test the cloning behavior used below.
  class MockNode : public fidl::WireServer<fio::Node> {
   public:
    explicit MockNode(MockBlockDevice* self) : self_(self) {}

    void Clone(CloneRequestView request, CloneCompleter::Sync& completer) override {
      self_->Bind(self_->dispatcher_, request->object.TakeChannel());
    }

    void Close(CloseRequestView request, CloseCompleter::Sync& completer) override {}
    void Close2(Close2RequestView request, Close2Completer::Sync& completer) override {}
    void Describe(DescribeRequestView request, DescribeCompleter::Sync& completer) override {}
    void Sync(SyncRequestView request, SyncCompleter::Sync& completer) override {}
    void Sync2(Sync2RequestView request, Sync2Completer::Sync& completer) override {}
    void GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) override {}
    void SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) override {}
    void GetFlags(GetFlagsRequestView request, GetFlagsCompleter::Sync& completer) override {}
    void SetFlags(SetFlagsRequestView request, SetFlagsCompleter::Sync& completer) override {}
    void QueryFilesystem(QueryFilesystemRequestView request,
                         QueryFilesystemCompleter::Sync& completer) override {}

   private:
    MockBlockDevice* self_;
  };

  zx_status_t BlockGetInfo(fidl_txn_t* txn) {
    fuchsia_hardware_block_BlockInfo info = {};
    return fuchsia_hardware_block_BlockGetInfo_reply(txn, ZX_OK, &info);
  }

  zx_status_t BlockGetStats(bool clear, fidl_txn_t* txn) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t BlockGetFifo(fidl_txn_t* txn) {
    fzl::fifo<block_fifo_request_t, block_fifo_response_t> client;
    EXPECT_EQ(fzl::create_fifo(BLOCK_FIFO_MAX_DEPTH, 0, &client, &fifo_), ZX_OK);
    return fuchsia_hardware_block_BlockGetFifo_reply(txn, ZX_OK, client.release());
  }

  zx_status_t BlockAttachVmo(zx_handle_t vmo, fidl_txn_t* txn) {
    fuchsia_hardware_block_VmoId vmoid = {kGoldenVmoid};
    return fuchsia_hardware_block_BlockAttachVmo_reply(txn, ZX_OK, &vmoid);
  }

  zx_status_t BlockCloseFifo(fidl_txn_t* txn) {
    fifo_.reset();
    return fuchsia_hardware_block_BlockCloseFifo_reply(txn, ZX_OK);
  }

  zx_status_t BlockRebindDevice(fidl_txn_t* txn) { return ZX_ERR_NOT_SUPPORTED; }

  async_dispatcher_t* dispatcher_ = nullptr;
  fzl::fifo<block_fifo_response_t, block_fifo_request_t> fifo_;
  std::unique_ptr<MockNode> mock_node_;
};

// Tests that the RemoteBlockDevice can be created and immediately destroyed.
TEST(RemoteBlockDeviceTest, Constructor) {
  zx::channel client, server;
  ASSERT_EQ(zx::channel::create(0, &client, &server), ZX_OK);

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  MockBlockDevice mock_device;
  ASSERT_EQ(mock_device.Bind(loop.dispatcher(), std::move(server)), ZX_OK);

  std::unique_ptr<RemoteBlockDevice> device;
  ASSERT_EQ(RemoteBlockDevice::Create(std::move(client), &device), ZX_OK);
}

// Tests that a fifo is attached to the block device for the duration of the
// RemoteBlockDevice lifetime.
TEST(RemoteBlockDeviceTest, FifoClosedOnDestruction) {
  zx::channel client, server;
  ASSERT_EQ(zx::channel::create(0, &client, &server), ZX_OK);

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  MockBlockDevice mock_device;
  ASSERT_EQ(mock_device.Bind(loop.dispatcher(), std::move(server)), ZX_OK);

  EXPECT_FALSE(mock_device.FifoAttached());
  {
    std::unique_ptr<RemoteBlockDevice> device;
    ASSERT_EQ(RemoteBlockDevice::Create(std::move(client), &device), ZX_OK);
    EXPECT_TRUE(mock_device.FifoAttached());
  }
  EXPECT_FALSE(mock_device.FifoAttached());
}

// Tests that the RemoteBlockDevice is capable of transmitting and receiving
// messages with the block device.
TEST(RemoteBlockDeviceTest, WriteTransactionReadResponse) {
  zx::channel client, server;
  ASSERT_EQ(zx::channel::create(0, &client, &server), ZX_OK);

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  MockBlockDevice mock_device;
  ASSERT_EQ(mock_device.Bind(loop.dispatcher(), std::move(server)), ZX_OK);

  std::unique_ptr<RemoteBlockDevice> device;
  ASSERT_EQ(RemoteBlockDevice::Create(std::move(client), &device), ZX_OK);

  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

  storage::OwnedVmoid vmoid;
  ASSERT_EQ(device->BlockAttachVmo(vmo, &vmoid.GetReference(device.get())), ZX_OK);
  ASSERT_EQ(kGoldenVmoid, vmoid.get());

  block_fifo_request_t request;
  request.opcode = BLOCKIO_READ;
  request.reqid = 1;
  request.group = 0;
  request.vmoid = vmoid.get();
  request.length = 1;
  request.vmo_offset = 0;
  request.dev_offset = 0;

  std::thread server_thread([&mock_device, &request] {
    block_fifo_request_t server_request;
    size_t actual;
    EXPECT_EQ(mock_device.ReadFifoRequests(&server_request, &actual), ZX_OK);
    EXPECT_EQ(actual, 1u);
    EXPECT_EQ(0, memcmp(&server_request, &request, sizeof(request)));

    block_fifo_response_t response;
    response.status = ZX_OK;
    response.reqid = request.reqid;
    response.group = request.group;
    response.count = 1;
    EXPECT_EQ(mock_device.WriteFifoResponse(response), ZX_OK);
  });

  ASSERT_EQ(device->FifoTransaction(&request, 1), ZX_OK);
  vmoid.TakeId();
  server_thread.join();
}

TEST(RemoteBlockDeviceTest, VolumeManagerOrdinals) {
  zx::channel client, server;
  ASSERT_EQ(zx::channel::create(0, &client, &server), ZX_OK);

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  MockBlockDevice mock_device;
  ASSERT_EQ(mock_device.Bind(loop.dispatcher(), std::move(server)), ZX_OK);

  std::unique_ptr<RemoteBlockDevice> device;
  ASSERT_EQ(RemoteBlockDevice::Create(std::move(client), &device), ZX_OK);

  // Querying the volume returns an error; the device doesn't implement
  // any FVM protocols. However, VolumeQuery utilizes a distinct
  // channel, so the connection should remain open.
  fuchsia_hardware_block_volume_VolumeManagerInfo manager_info;
  fuchsia_hardware_block_volume_VolumeInfo volume_info;
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, device->VolumeGetInfo(&manager_info, &volume_info));

  // Other block functions still function correctly.
  fuchsia_hardware_block_BlockInfo block_info;
  EXPECT_EQ(device->BlockGetInfo(&block_info), ZX_OK);

  // Sending any FVM method other than "VolumeQuery" also returns an error.
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, device->VolumeExtend(0, 0));

  // But now, other (previously valid) block methods fail, because FIDL has
  // closed the channel.
  EXPECT_EQ(ZX_ERR_PEER_CLOSED, device->BlockGetInfo(&block_info));
}

TEST(RemoteBlockDeviceTest, LargeThreadCountSuceeds) {
  zx::channel client, server;
  ASSERT_EQ(zx::channel::create(0, &client, &server), ZX_OK);

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  MockBlockDevice mock_device;
  ASSERT_EQ(mock_device.Bind(loop.dispatcher(), std::move(server)), ZX_OK);

  std::unique_ptr<RemoteBlockDevice> device;
  ASSERT_EQ(RemoteBlockDevice::Create(std::move(client), &device), ZX_OK);

  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

  storage::OwnedVmoid vmoid;
  ASSERT_EQ(device->BlockAttachVmo(vmo, &vmoid.GetReference(device.get())), ZX_OK);
  ASSERT_EQ(kGoldenVmoid, vmoid.get());

  constexpr int kThreadCount = 2 * MAX_TXN_GROUP_COUNT;
  std::thread threads[kThreadCount];
  fbl::Mutex mutex;
  fbl::ConditionVariable condition;
  int done = 0;
  for (int i = 0; i < kThreadCount; ++i) {
    threads[i] =
        std::thread([device = device.get(), &mutex, &done, &condition, vmoid = vmoid.get()]() {
          block_fifo_request_t request = {};
          request.opcode = BLOCKIO_READ;
          request.vmoid = vmoid;
          request.length = 1;
          ASSERT_EQ(device->FifoTransaction(&request, 1), ZX_OK);
          fbl::AutoLock lock(&mutex);
          ++done;
          condition.Signal();
        });
  }
  vmoid.TakeId();  // We don't need the vmoid any more.
  block_fifo_request_t requests[kThreadCount + BLOCK_FIFO_MAX_DEPTH];
  size_t request_count = 0;
  do {
    if (request_count < kThreadCount) {
      // Read some more requests.
      size_t count = 0;
      ASSERT_EQ(mock_device.ReadFifoRequests(&requests[request_count], &count), ZX_OK);
      ASSERT_GT(count, 0u);
      request_count += count;
    }
    // Check that all the outstanding requests we have use different group IDs.
    std::unordered_set<groupid_t> groups;
    for (size_t i = done; i < request_count; ++i) {
      ASSERT_TRUE(groups.insert(requests[i].group).second);
    }
    // Finish one request.
    block_fifo_response_t response;
    response.status = ZX_OK;
    response.reqid = requests[done].reqid;
    response.group = requests[done].group;
    response.count = 1;
    int last_done = done;
    EXPECT_EQ(mock_device.WriteFifoResponse(response), ZX_OK);
    // Wait for it to be done.
    fbl::AutoLock lock(&mutex);
    while (done != last_done + 1) {
      condition.Wait(&mutex);
    }
  } while (done < kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads[i].join();
  }
}

TEST(RemoteBlockDeviceTest, NoHangForErrorsWithMultipleThreads) {
  zx::channel client, server;
  ASSERT_EQ(zx::channel::create(0, &client, &server), ZX_OK);
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);
  std::unique_ptr<RemoteBlockDevice> device;
  constexpr int kThreadCount = 4 * MAX_TXN_GROUP_COUNT;
  std::thread threads[kThreadCount];

  {
    MockBlockDevice mock_device;
    ASSERT_EQ(mock_device.Bind(loop.dispatcher(), std::move(server)), ZX_OK);

    ASSERT_EQ(RemoteBlockDevice::Create(std::move(client), &device), ZX_OK);

    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo), ZX_OK);

    storage::OwnedVmoid vmoid;
    ASSERT_EQ(device->BlockAttachVmo(vmo, &vmoid.GetReference(device.get())), ZX_OK);
    ASSERT_EQ(kGoldenVmoid, vmoid.get());

    for (int i = 0; i < kThreadCount; ++i) {
      threads[i] = std::thread([device = device.get(), vmoid = vmoid.get()]() {
        block_fifo_request_t request = {};
        request.opcode = BLOCKIO_READ;
        request.vmoid = vmoid;
        request.length = 1;
        ASSERT_EQ(ZX_ERR_PEER_CLOSED, device->FifoTransaction(&request, 1));
      });
    }
    vmoid.TakeId();  // We don't need the vmoid any more.

    // Wait for at least 2 requests to be received.
    block_fifo_request_t requests[BLOCK_FIFO_MAX_DEPTH];
    size_t request_count = 0;
    while (request_count < 2) {
      size_t count = 0;
      ASSERT_EQ(mock_device.ReadFifoRequests(requests, &count), ZX_OK);
      request_count += count;
    }

    // Allow MockBlockDevice to go out of scope which should close the fifo.
    loop.Shutdown();
  }

  // We should be able to join all the threads.
  for (int i = 0; i < kThreadCount; ++i) {
    threads[i].join();
  }
}

}  // namespace
}  // namespace block_client
