// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fdio/directory.h>

#include <vector>

#include <fs-management/admin.h>
#include <fs-management/format.h>
#include <gtest/gtest.h>
#include <ramdevice-client/ramdisk.h>

namespace fio = fuchsia_io;

enum State {
  kEmpty,
  kFormatted,
  kStarted,
};

class OutgoingDirectoryTest : public testing::Test {
 public:
  explicit OutgoingDirectoryTest(disk_format_t format) : format_(format) {}

  void SetUp() final {
    ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk_), ZX_OK);
    const char* ramdisk_path = ramdisk_get_path(ramdisk_);

    ASSERT_EQ(mkfs(ramdisk_path, format_, launch_stdio_sync, MkfsOptions()), ZX_OK);
    state_ = kFormatted;
  }

  void TearDown() final {
    if (state_ == kStarted) {
      StopFilesystem();
    }
    ASSERT_EQ(ramdisk_destroy(ramdisk_), 0);
  }

 protected:
  void GetExportRoot(zx::unowned_channel* root) {
    ASSERT_EQ(state_, kStarted);
    *root = zx::unowned(export_root_);
  }

  void GetDataRoot(zx::channel* root) {
    ASSERT_EQ(state_, kStarted);
    ASSERT_EQ(fs_root_handle(export_root_.get(), root->reset_and_get_address()), ZX_OK);
  }

  void CheckDataRoot() {
    const char* format_str = disk_format_string(format_);
    zx::channel data_root;
    GetDataRoot(&data_root);
    fidl::WireSyncClient<fio::DirectoryAdmin> data_client(std::move(data_root));
    auto resp = data_client.QueryFilesystem();
    ASSERT_TRUE(resp.ok());
    ASSERT_EQ(resp.value().s, ZX_OK);
    ASSERT_EQ(strncmp(format_str, reinterpret_cast<char*>(resp.value().info->name.data()),
                      strlen(format_str)),
              0);
  }

  void StartFilesystem(const InitOptions& options) {
    ASSERT_EQ(state_, kFormatted);

    zx::channel device, device_server;
    const char* ramdisk_path = ramdisk_get_path(ramdisk_);
    ASSERT_EQ(zx::channel::create(0, &device, &device_server), ZX_OK);
    ASSERT_EQ(fdio_service_connect(ramdisk_path, device_server.release()), ZX_OK);

    ASSERT_EQ(fs_init(device.release(), format_, options, export_root_.reset_and_get_address()),
              ZX_OK);
    state_ = kStarted;
  }

  void StopFilesystem() {
    ASSERT_EQ(state_, kStarted);
    zx::channel data_root;
    GetDataRoot(&data_root);

    fidl::WireSyncClient<fio::DirectoryAdmin> data_client(std::move(data_root));
    auto resp = data_client.Unmount();
    ASSERT_TRUE(resp.ok());
    ASSERT_EQ(resp.value().s, ZX_OK);

    state_ = kFormatted;
  }

  void WriteTestFile() {
    ASSERT_EQ(state_, kStarted);
    zx::channel data_root;
    GetDataRoot(&data_root);
    fidl::WireSyncClient<fio::Directory> data_client(std::move(data_root));

    zx::channel test_file, test_file_server;
    ASSERT_EQ(zx::channel::create(0, &test_file, &test_file_server), ZX_OK);
    uint32_t file_flags =
        fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable | fio::wire::kOpenFlagCreate;
    ASSERT_EQ(data_client.Open(file_flags, 0, "test_file", std::move(test_file_server)).status(),
              ZX_OK);

    fidl::WireSyncClient<fio::File> file_client(std::move(test_file));
    std::vector<uint8_t> content{1, 2, 3, 4};
    auto resp = file_client.Write(fidl::VectorView<uint8_t>::FromExternal(content));
    ASSERT_EQ(resp.status(), ZX_OK);
    ASSERT_EQ(resp.value().s, ZX_OK);
    ASSERT_EQ(resp.value().actual, content.size());

    auto resp2 = file_client.Close();
    ASSERT_EQ(resp2.status(), ZX_OK);
    ASSERT_EQ(resp2.value().s, ZX_OK);
  }

 private:
  State state_ = kEmpty;
  ramdisk_client_t* ramdisk_ = nullptr;
  zx::channel export_root_;
  disk_format_t format_;
};

static constexpr InitOptions kReadonlyOptions = {
    .readonly = true,
    .verbose_mount = false,
    .collect_metrics = false,
    .wait_until_ready = true,
    .write_compression_algorithm = nullptr,
    .write_compression_level = -1,
    .callback = launch_stdio_async,
};

class OutgoingDirectoryBlobfs : public OutgoingDirectoryTest {
 public:
  OutgoingDirectoryBlobfs() : OutgoingDirectoryTest(DISK_FORMAT_BLOBFS) {}
};

class OutgoingDirectoryMinfs : public OutgoingDirectoryTest {
 public:
  OutgoingDirectoryMinfs() : OutgoingDirectoryTest(DISK_FORMAT_MINFS) {}
};

TEST_F(OutgoingDirectoryBlobfs, OutgoingDirectoryReadWriteDataRootIsValidBlobfs) {
  StartFilesystem(InitOptions());
  CheckDataRoot();
}

TEST_F(OutgoingDirectoryBlobfs, OutgoingDirectoryReadOnlyDataRootIsValidBlobfs) {
  StartFilesystem(kReadonlyOptions);
  CheckDataRoot();
}

TEST_F(OutgoingDirectoryMinfs, OutgoingDirectoryReadWriteDataRootIsValidMinfs) {
  StartFilesystem(InitOptions());
  CheckDataRoot();
}

TEST_F(OutgoingDirectoryMinfs, OutgoingDirectoryReadOnlyDataRootIsValidMinfs) {
  StartFilesystem(kReadonlyOptions);
  CheckDataRoot();
}

TEST_F(OutgoingDirectoryMinfs, CanWriteToReadWriteMinfsDataRoot) {
  StartFilesystem(InitOptions());
  WriteTestFile();
}

TEST_F(OutgoingDirectoryMinfs, CannotWriteToReadOnlyMinfsDataRoot) {
  // write an initial test file onto a writable filesystem
  StartFilesystem(InitOptions());
  WriteTestFile();
  StopFilesystem();

  // start the filesystem in read-only mode
  StartFilesystem(kReadonlyOptions);
  zx::channel data_root;
  GetDataRoot(&data_root);
  fidl::WireSyncClient<fio::Directory> data_client(std::move(data_root));

  zx::channel fail_test_file, fail_test_file_server;
  ASSERT_EQ(zx::channel::create(0, &fail_test_file, &fail_test_file_server), ZX_OK);
  uint32_t fail_file_flags = fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable;
  // open "succeeds" but...
  ASSERT_EQ(
      data_client.Open(fail_file_flags, 0, "test_file", std::move(fail_test_file_server)).status(),
      ZX_OK);

  // ...we can't actually use the channel
  fidl::WireSyncClient<fio::File> fail_file_client(std::move(fail_test_file));
  auto resp = fail_file_client.Read(4);
  ASSERT_EQ(resp.status(), ZX_ERR_PEER_CLOSED);

  // the channel will be valid if we open the file read-only though
  zx::channel test_file, test_file_server;
  ASSERT_EQ(zx::channel::create(0, &test_file, &test_file_server), ZX_OK);
  uint32_t file_flags = fio::wire::kOpenRightReadable;
  ASSERT_EQ(data_client.Open(file_flags, 0, "test_file", std::move(test_file_server)).status(),
            ZX_OK);

  fidl::WireSyncClient<fio::File> file_client(std::move(test_file));
  auto resp2 = file_client.Read(4);
  ASSERT_EQ(resp2.status(), ZX_OK);
  ASSERT_EQ(resp2.value().s, ZX_OK);
  ASSERT_EQ(resp2.value().data.data()[0], 1);

  auto resp3 = file_client.Close();
  ASSERT_EQ(resp3.status(), ZX_OK);
  ASSERT_EQ(resp3.value().s, ZX_OK);
}

TEST_F(OutgoingDirectoryMinfs, CannotWriteToOutgoingDirectory) {
  StartFilesystem(InitOptions());
  zx::unowned_channel export_root;
  GetExportRoot(&export_root);

  auto test_file_name = std::string("test_file");
  zx::channel test_file, test_file_server;
  ASSERT_EQ(zx::channel::create(0, &test_file, &test_file_server), ZX_OK);
  uint32_t file_flags =
      fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable | fio::wire::kOpenFlagCreate;
  ASSERT_EQ(fidl::WireCall<fio::Directory>(std::move(export_root))
                .Open(file_flags, 0, fidl::StringView::FromExternal(test_file_name),
                      std::move(test_file_server))
                .status(),
            ZX_OK);

  fidl::WireSyncClient<fio::File> file_client(std::move(test_file));
  std::vector<uint8_t> content{1, 2, 3, 4};
  auto resp = file_client.Write(fidl::VectorView<uint8_t>::FromExternal(content));
  ASSERT_EQ(resp.status(), ZX_ERR_PEER_CLOSED);
}
