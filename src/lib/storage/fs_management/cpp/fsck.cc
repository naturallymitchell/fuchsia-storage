// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.fs.startup/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/limits.h>
#include <lib/fdio/vfs.h>
#include <lib/service/llcpp/service.h>
#include <lib/zx/channel.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <new>

#include <fbl/unique_fd.h>
#include <fbl/vector.h>

#include "src/lib/storage/fs_management/cpp/component.h"
#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/lib/storage/fs_management/cpp/path.h"

namespace fs_management {
namespace {

zx_status_t FsckNativeFs(const char* device_path, const FsckOptions& options, LaunchCallback cb,
                         const char* binary) {
  zx::channel crypt_client(options.crypt_client);
  fbl::unique_fd device_fd;
  device_fd.reset(open(device_path, O_RDWR));
  if (!device_fd) {
    fprintf(stderr, "Failed to open device\n");
    return ZX_ERR_BAD_STATE;
  }
  zx::channel block_device;
  zx_status_t status =
      fdio_get_service_handle(device_fd.release(), block_device.reset_and_get_address());
  if (status != ZX_OK) {
    return status;
  }

  std::vector<std::pair<uint32_t, zx::handle>> handles;
  handles.push_back({FS_HANDLE_BLOCK_DEVICE_ID, std::move(block_device)});
  if (crypt_client) {
    handles.push_back({PA_HND(PA_USER0, 2), std::move(crypt_client)});
  }
  return cb(options.as_argv(binary), std::move(handles));
}

zx_status_t FsckFat(const char* device_path, const FsckOptions& options, LaunchCallback cb) {
  return cb(options.as_argv_fat32(GetBinaryPath("fsck-msdosfs").c_str(), device_path), {});
}

zx::status<> FsckComponentFs(fidl::UnownedClientEnd<fuchsia_io::Directory> exposed_dir,
                             const std::string& device_path, const FsckOptions& options) {
  zx::channel crypt_client(options.crypt_client);

  auto device = service::Connect<fuchsia_hardware_block::Block>(device_path.c_str());
  if (device.is_error())
    return device.take_error();

  auto startup_client_end = service::ConnectAt<fuchsia_fs_startup::Startup>(exposed_dir);
  if (startup_client_end.is_error())
    return startup_client_end.take_error();
  auto startup_client = fidl::BindSyncClient(std::move(*startup_client_end));

  auto res = startup_client->Check(std::move(*device), options.as_check_options());
  if (!res.ok())
    return zx::error(res.status());
  if (res->result.is_err())
    return zx::error(res->result.err());

  return zx::ok();
}

}  // namespace

__EXPORT
zx_status_t Fsck(std::string_view device_path, DiskFormat df, const FsckOptions& options,
                 LaunchCallback cb) {
  std::string device_path_str(device_path);
  // N.B. Make sure to release crypt_client in any new error paths here.
  if (options.component_child_name != nullptr) {
    std::string_view url = DiskFormatComponentUrl(df);
    // If we don't know the url, fall back on the old launching method.
    if (!url.empty()) {
      // Otherwise, launch the component way.
      auto exposed_dir_or = ConnectNativeFsComponent(url, options.component_child_name,
                                                     options.component_collection_name);
      if (exposed_dir_or.is_error()) {
        if (options.crypt_client != ZX_HANDLE_INVALID)
          zx_handle_close(options.crypt_client);
        return exposed_dir_or.status_value();
      }
      return FsckComponentFs(*exposed_dir_or, device_path_str, options).status_value();
    }
  }

  switch (df) {
    case kDiskFormatFactoryfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("factoryfs").c_str());
    case kDiskFormatMinfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("minfs").c_str());
    case kDiskFormatFxfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("fxfs").c_str());
    case kDiskFormatFat:
      if (options.crypt_client != ZX_HANDLE_INVALID)
        zx_handle_close(options.crypt_client);
      return FsckFat(device_path_str.c_str(), options, cb);
    case kDiskFormatBlobfs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("blobfs").c_str());
    case kDiskFormatF2fs:
      return FsckNativeFs(device_path_str.c_str(), options, cb, GetBinaryPath("f2fs").c_str());
    default:
      auto* format = CustomDiskFormat::Get(df);
      if (format == nullptr) {
        if (options.crypt_client != ZX_HANDLE_INVALID)
          zx_handle_close(options.crypt_client);
        return ZX_ERR_NOT_SUPPORTED;
      }
      return FsckNativeFs(device_path_str.c_str(), options, cb, format->binary_path().c_str());
  }
}

}  // namespace fs_management
