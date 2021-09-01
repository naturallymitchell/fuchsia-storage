// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_INCLUDE_FS_MANAGEMENT_FORMAT_H_
#define SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_INCLUDE_FS_MANAGEMENT_FORMAT_H_

#include <zircon/types.h>

__BEGIN_CDECLS

typedef enum disk_format_type {
  DISK_FORMAT_UNKNOWN = 0,
  DISK_FORMAT_GPT = 1,
  DISK_FORMAT_MBR = 2,
  DISK_FORMAT_MINFS = 3,
  DISK_FORMAT_FAT = 4,
  DISK_FORMAT_BLOBFS = 5,
  DISK_FORMAT_FVM = 6,
  DISK_FORMAT_ZXCRYPT = 7,
  DISK_FORMAT_FACTORYFS = 8,
  DISK_FORMAT_BLOCK_VERITY = 9,
  DISK_FORMAT_VBMETA = 10,
  DISK_FORMAT_BOOTPART = 11,
  DISK_FORMAT_FXFS = 12,
  DISK_FORMAT_F2FS = 13,
  DISK_FORMAT_COUNT_,
} disk_format_t;

const char* disk_format_string(disk_format_t fs_type);

#define HEADER_SIZE 4096

static const uint8_t minfs_magic[16] = {
    0x21, 0x4d, 0x69, 0x6e, 0x46, 0x53, 0x21, 0x00, 0x04, 0xd3, 0xd3, 0xd3, 0xd3, 0x00, 0x50, 0x38,
};

static const uint8_t blobfs_magic[16] = {
    0x21, 0x4d, 0x69, 0x9e, 0x47, 0x53, 0x21, 0xac, 0x14, 0xd3, 0xd3, 0xd4, 0xd4, 0x00, 0x50, 0x98,
};

static const uint8_t gpt_magic[16] = {
    0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54, 0x00, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x00,
};

static const uint8_t fvm_magic[8] = {
    0x46, 0x56, 0x4d, 0x20, 0x50, 0x41, 0x52, 0x54,
};

static const uint8_t zxcrypt_magic[16] = {
    0x5f, 0xe8, 0xf8, 0x00, 0xb3, 0x6d, 0x11, 0xe7, 0x80, 0x7a, 0x78, 0x63, 0x72, 0x79, 0x70, 0x74,
};

static const uint8_t block_verity_magic[16] = {0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x2d, 0x76, 0x65,
                                               0x72, 0x69, 0x74, 0x79, 0x2d, 0x76, 0x31, 0x00};
static const uint8_t factoryfs_magic[8] = {
    0x21, 0x4d, 0x69, 0x1e, 0xF9, 0x3F, 0x5D, 0xA5,
};

static const uint8_t vbmeta_magic[4] = {
    'A',
    'V',
    'B',
    '0',
};

static const uint8_t f2fs_magic[4] = {
    0x10,
    0x20,
    0xf5,
    0xf2,
};

disk_format_t detect_disk_format(int fd);
disk_format_t detect_disk_format_log_unknown(int fd);

__END_CDECLS

#ifdef __cplusplus

#include <memory>
#include <string>

namespace fs_management {

class __EXPORT CustomDiskFormat {
 public:
  static disk_format_t Register(std::unique_ptr<CustomDiskFormat> format);
  static const CustomDiskFormat* Get(disk_format_t);

  CustomDiskFormat(std::string name, std::string_view binary_path)
      : name_(std::move(name)), binary_path_(binary_path) {}
  CustomDiskFormat(CustomDiskFormat&&) = default;

  const std::string& name() const { return name_; }
  const std::string& binary_path() const { return binary_path_; }

 private:
  std::string name_;
  std::string binary_path_;
};

}  // namespace fs_management

#endif

#endif  // SRC_LIB_STORAGE_FS_MANAGEMENT_CPP_INCLUDE_FS_MANAGEMENT_FORMAT_H_
