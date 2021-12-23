// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io.admin/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>

#include <vector>

#include <fs-management/admin.h>
#include <fs-management/format.h>
#include <fs-management/mount.h>
#include <gtest/gtest.h>
#include <ramdevice-client/ramdisk.h>

#include "src/storage/testing/ram_disk.h"

namespace fs_management {
namespace {

namespace fio = fuchsia_io;
using fuchsia_io_admin::DirectoryAdmin;

enum State {
  kFormatted,
  kStarted,
};

static constexpr InitOptions kReadonlyOptions = {
    .readonly = true,
    // Remaining options are same as default values.
};

constexpr char kTestFilePath[] = "test_file";

class OutgoingDirectoryFixture : public testing::Test {
 public:
  explicit OutgoingDirectoryFixture(DiskFormat format, InitOptions options = {})
      : format_(format), options_(options) {}

  void SetUp() override {
    ASSERT_EQ(storage::WaitForRamctl().status_value(), ZX_OK);
    zx_status_t status;
    ASSERT_EQ(status = ramdisk_create(512, 1 << 16, &ramdisk_), ZX_OK)
        << zx_status_get_string(status);
    const char* ramdisk_path = ramdisk_get_path(ramdisk_);

    ASSERT_EQ(status = Mkfs(ramdisk_path, format_, launch_stdio_sync, MkfsOptions()), ZX_OK)
        << zx_status_get_string(status);
    state_ = kFormatted;

    ASSERT_NO_FATAL_FAILURE(StartFilesystem(options_));
  }

  void TearDown() final {
    ASSERT_NO_FATAL_FAILURE(StopFilesystem());
    zx_status_t status;
    ASSERT_EQ(status = ramdisk_destroy(ramdisk_), ZX_OK) << zx_status_get_string(status);
  }

  fidl::WireSyncClient<DirectoryAdmin>& DataRoot() {
    ZX_ASSERT(state_ == kStarted);  // Ensure this isn't used after stopping the filesystem.
    return data_client_;
  }

  fidl::WireSyncClient<DirectoryAdmin>& ExportRoot() {
    ZX_ASSERT(state_ == kStarted);  // Ensure this isn't used after stopping the filesystem.
    return export_client_;
  }

 protected:
  void StartFilesystem(const InitOptions& options) {
    ASSERT_EQ(state_, kFormatted);

    const char* ramdisk_path = ramdisk_get_path(ramdisk_);
    ASSERT_NE(ramdisk_path, nullptr);

    zx_status_t status;
    zx::channel device, device_server;
    ASSERT_EQ(status = zx::channel::create(0, &device, &device_server), ZX_OK)
        << zx_status_get_string(status);
    ASSERT_EQ(status = fdio_service_connect(ramdisk_path, device_server.release()), ZX_OK)
        << zx_status_get_string(status);

    auto export_root = FsInit(std::move(device), format_, options);
    ASSERT_TRUE(export_root.is_ok()) << export_root.status_string();
    export_client_ = fidl::WireSyncClient<DirectoryAdmin>(std::move(export_root.value()));

    auto data_root = FsRootHandle(export_client_.client_end());
    ASSERT_TRUE(data_root.is_ok()) << data_root.status_string();
    data_client_ = fidl::WireSyncClient<DirectoryAdmin>(std::move(data_root.value()));

    state_ = kStarted;
  }

  void StopFilesystem() {
    if (state_ != kStarted) {
      return;
    }
    ASSERT_EQ(fs_management::Shutdown(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                          export_client_.client_end()))
                  .status_value(),
              ZX_OK);

    state_ = kFormatted;
  }

 private:
  State state_ = kFormatted;
  ramdisk_client_t* ramdisk_ = nullptr;
  DiskFormat format_;
  InitOptions options_ = {};
  fidl::WireSyncClient<DirectoryAdmin> export_client_;
  fidl::WireSyncClient<DirectoryAdmin> data_client_;
};

// Generalized Admin Tests

struct OutgoingDirectoryTestParameters {
  DiskFormat format;
  InitOptions options;
};

std::ostream& operator<<(std::ostream& out, const OutgoingDirectoryTestParameters& params) {
  out << DiskFormatString(params.format);
  if (params.options.readonly) {
    out << "_readonly";
  }
  return out;
}

// Generalized outgoing directory tests which should work in both mutable and read-only modes.
class OutgoingDirectoryTest : public OutgoingDirectoryFixture,
                              public testing::WithParamInterface<OutgoingDirectoryTestParameters> {
 public:
  OutgoingDirectoryTest() : OutgoingDirectoryFixture(GetParam().format, GetParam().options) {}
};

TEST_P(OutgoingDirectoryTest, DataRootIsValid) {
  std::string_view format_str = DiskFormatString(GetParam().format);
  auto resp = DataRoot()->QueryFilesystem();
  ASSERT_TRUE(resp.ok()) << resp.status_string();
  ASSERT_EQ(resp.value().s, ZX_OK) << zx_status_get_string(resp.value().s);
  ASSERT_STREQ(format_str.data(), reinterpret_cast<char*>(resp.value().info->name.data()));
}

INSTANTIATE_TEST_SUITE_P(
    OutgoingDirectoryTest, OutgoingDirectoryTest,
    testing::Values(OutgoingDirectoryTestParameters{kDiskFormatBlobfs, {}},
                    OutgoingDirectoryTestParameters{kDiskFormatBlobfs, kReadonlyOptions},
                    OutgoingDirectoryTestParameters{kDiskFormatMinfs, {}},
                    OutgoingDirectoryTestParameters{kDiskFormatMinfs, kReadonlyOptions}),
    testing::PrintToStringParamName());

// Minfs-Specific Tests (can be generalized to work with any mutable filesystem by parameterizing
// on the disk format if required).
// Launches the filesystem and creates a file called kTestFilePath in the data root.
class OutgoingDirectoryMinfs : public OutgoingDirectoryFixture {
 public:
  OutgoingDirectoryMinfs() : OutgoingDirectoryFixture(kDiskFormatMinfs) {}

  void SetUp() final {
    // Make sure we invoke the base fixture's SetUp method before we continue.
    ASSERT_NO_FATAL_FAILURE(OutgoingDirectoryFixture::SetUp());
    // Since we initialize the fixture with the default writable options, we should always
    // be able to create an initial test file.
    ASSERT_NO_FATAL_FAILURE(WriteTestFile());
  }

 private:
  void WriteTestFile() {
    auto test_file_ends = fidl::CreateEndpoints<fio::File>();
    ASSERT_TRUE(test_file_ends.is_ok()) << test_file_ends.status_string();
    fidl::ServerEnd<fio::Node> test_file_server(test_file_ends->server.TakeChannel());

    uint32_t file_flags =
        fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable | fio::wire::kOpenFlagCreate;
    ASSERT_EQ(DataRoot()->Open(file_flags, 0, kTestFilePath, std::move(test_file_server)).status(),
              ZX_OK);

    fidl::WireSyncClient<fio::File> file_client(std::move(test_file_ends->client));
    std::vector<uint8_t> content{1, 2, 3, 4};
    auto resp = file_client->Write(fidl::VectorView<uint8_t>::FromExternal(content));
    ASSERT_TRUE(resp.ok()) << resp.status_string();
    ASSERT_EQ(resp.value().s, ZX_OK) << zx_status_get_string(resp.value().s);
    ASSERT_EQ(resp.value().actual, content.size());

    auto resp2 = file_client->Close();
    ASSERT_TRUE(resp2.ok()) << resp2.status_string();
    ASSERT_EQ(resp2.value().s, ZX_OK) << zx_status_get_string(resp2.value().s);
  }
};

TEST_F(OutgoingDirectoryMinfs, CannotWriteToReadOnlyDataRoot) {
  // restart the filesystem in read-only mode
  ASSERT_NO_FATAL_FAILURE(StopFilesystem());
  ASSERT_NO_FATAL_FAILURE(StartFilesystem(kReadonlyOptions));

  auto fail_file_ends = fidl::CreateEndpoints<fio::File>();
  ASSERT_TRUE(fail_file_ends.is_ok()) << fail_file_ends.status_string();
  fidl::ServerEnd<fio::Node> fail_test_file_server(fail_file_ends->server.TakeChannel());

  uint32_t fail_file_flags = fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable;
  // open "succeeds" but...
  auto open_resp =
      DataRoot()->Open(fail_file_flags, 0, kTestFilePath, std::move(fail_test_file_server));
  ASSERT_TRUE(open_resp.ok()) << open_resp.status_string();

  // ...we can't actually use the channel
  fidl::WireSyncClient<fio::File> fail_file_client(std::move(fail_file_ends->client));
  auto resp = fail_file_client->Read(4);
  ASSERT_EQ(resp.status(), ZX_ERR_PEER_CLOSED);

  // the channel will be valid if we open the file read-only though
  auto test_file_ends = fidl::CreateEndpoints<fio::File>();
  ASSERT_TRUE(test_file_ends.is_ok()) << test_file_ends.status_string();
  fidl::ServerEnd<fio::Node> test_file_server(test_file_ends->server.TakeChannel());

  uint32_t file_flags = fio::wire::kOpenRightReadable;
  auto open_resp2 = DataRoot()->Open(file_flags, 0, kTestFilePath, std::move(test_file_server));
  ASSERT_TRUE(open_resp2.ok()) << open_resp2.status_string();

  fidl::WireSyncClient<fio::File> file_client(std::move(test_file_ends->client));
  auto resp2 = file_client->Read(4);
  ASSERT_EQ(resp2.status(), ZX_OK) << resp2.status_string();
  ASSERT_EQ(resp2.value().s, ZX_OK) << zx_status_get_string(resp2.value().s);
  ASSERT_EQ(resp2.value().data.data()[0], 1);

  auto close_resp = file_client->Close();
  ASSERT_TRUE(close_resp.ok()) << close_resp.status_string();
  ASSERT_EQ(close_resp.value().s, ZX_OK) << zx_status_get_string(close_resp.value().s);
}

TEST_F(OutgoingDirectoryMinfs, CannotWriteToOutgoingDirectory) {
  auto test_file_ends = fidl::CreateEndpoints<fio::File>();
  ASSERT_TRUE(test_file_ends.is_ok()) << test_file_ends.status_string();
  fidl::ServerEnd<fio::Node> test_file_server(test_file_ends->server.TakeChannel());

  uint32_t file_flags =
      fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable | fio::wire::kOpenFlagCreate;
  auto open_resp = ExportRoot()->Open(file_flags, 0, kTestFilePath, std::move(test_file_server));
  ASSERT_TRUE(open_resp.ok()) << open_resp.status_string();

  fidl::WireSyncClient<fio::File> file_client(std::move(test_file_ends->client));
  std::vector<uint8_t> content{1, 2, 3, 4};
  auto write_resp = file_client->Write(fidl::VectorView<uint8_t>::FromExternal(content));
  ASSERT_EQ(write_resp.status(), ZX_ERR_PEER_CLOSED) << write_resp.status_string();
}

}  // namespace
}  // namespace fs_management
