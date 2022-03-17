// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/limits.h>
#include <lib/fdio/vfs.h>
#include <lib/zx/channel.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/device/vfs.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iterator>
#include <new>
#include <vector>

#include <fbl/algorithm.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>

#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/lib/storage/fs_management/cpp/path.h"

namespace fs_management {
namespace {

zx_status_t MkfsNativeFs(const char* binary, const char* device_path, LaunchCallback cb,
                         const MkfsOptions& options, bool support_fvm) {
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
  std::vector<std::string> argv_strings = options.as_argv(binary);
  int argc = static_cast<int>(argv_strings.size());
  std::vector<const char*> argv;
  argv.reserve(argv_strings.size());
  for (const std::string& arg : argv_strings) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(nullptr);

  zx_handle_t handles[] = {block_device.release(), crypt_client.release()};
  uint32_t ids[] = {FS_HANDLE_BLOCK_DEVICE_ID, PA_HND(PA_USER0, 2)};
  return cb(argc, argv.data(), handles, ids, handles[1] == ZX_HANDLE_INVALID ? 1 : 2);
}

zx_status_t MkfsFat(const char* device_path, LaunchCallback cb, const MkfsOptions& options) {
  const std::string tool_path = GetBinaryPath("mkfs-msdosfs");
  std::string sectors_per_cluster;
  std::vector<const char*> argv = {tool_path.c_str()};
  if (options.sectors_per_cluster != 0) {
    argv.push_back("-c");
    sectors_per_cluster = std::to_string(options.sectors_per_cluster);
    argv.push_back(sectors_per_cluster.c_str());
  }
  argv.push_back(device_path);
  argv.push_back(nullptr);
  return cb(static_cast<int>(argv.size() - 1), argv.data(), NULL, NULL, 0);
}

}  // namespace

__EXPORT
zx_status_t Mkfs(const char* device_path, DiskFormat df, LaunchCallback cb,
                 const MkfsOptions& options) {
  // N.B. Make sure to release crypt_client in any new error paths here.
  switch (df) {
    case kDiskFormatFactoryfs:
      return MkfsNativeFs(GetBinaryPath("factoryfs").c_str(), device_path, cb, options, false);
    case kDiskFormatMinfs:
      return MkfsNativeFs(GetBinaryPath("minfs").c_str(), device_path, cb, options, true);
    case kDiskFormatFxfs:
      return MkfsNativeFs(GetBinaryPath("fxfs").c_str(), device_path, cb, options, true);
    case kDiskFormatFat:
      return MkfsFat(device_path, cb, options);
    case kDiskFormatBlobfs:
      return MkfsNativeFs(GetBinaryPath("blobfs").c_str(), device_path, cb, options, true);
    case kDiskFormatF2fs:
      return MkfsNativeFs(GetBinaryPath("f2fs").c_str(), device_path, cb, options, true);
    default:
      auto* format = CustomDiskFormat::Get(df);
      if (format == nullptr) {
        if (options.crypt_client != ZX_HANDLE_INVALID)
          zx_handle_close(options.crypt_client);
        return ZX_ERR_NOT_SUPPORTED;
      }
      return MkfsNativeFs(format->binary_path().c_str(), device_path, cb, options, true);
  }
}

}  // namespace fs_management
