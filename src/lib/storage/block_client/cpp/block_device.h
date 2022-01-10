// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_BLOCK_CLIENT_CPP_BLOCK_DEVICE_H_
#define SRC_LIB_STORAGE_BLOCK_CLIENT_CPP_BLOCK_DEVICE_H_

#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <lib/zx/status.h>
#include <lib/zx/vmo.h>

#include <memory>

#include <storage/buffer/vmoid_registry.h>

#include "src/lib/storage/block_client/cpp/client.h"

namespace block_client {

// An interface which virtualizes the connection to the underlying block device.
class BlockDevice : public storage::VmoidRegistry {
 public:
  // Reads from the block device using the fuchsia_io::File protocol.
  //
  // TODO(fxbug.dev/33909): Deprecate this interface. Favor reading over the FIFO protocol instead.
  virtual zx_status_t ReadBlock(uint64_t block_num, uint64_t block_size, void* block) const = 0;

  // FIFO protocol.
  virtual zx_status_t FifoTransaction(block_fifo_request_t* requests, size_t count) = 0;

  // Controller IPC.
  virtual zx::status<std::string> GetDevicePath() const = 0;

  // Block IPC.
  virtual zx_status_t BlockGetInfo(fuchsia_hardware_block_BlockInfo* out_info) const = 0;

  // A default implementation is provided that should work in most if not all cases.
  zx_status_t BlockDetachVmo(storage::Vmoid vmoid) override;

  // Volume IPC.
  //
  // VolumeGetInfo is safe to invoke, even for devices which do not necessarily speak the Volume
  // protocol.
  virtual zx_status_t VolumeGetInfo(
      fuchsia_hardware_block_volume_VolumeManagerInfo* out_manager_info,
      fuchsia_hardware_block_volume_VolumeInfo* out_volume_info) const = 0;
  virtual zx_status_t VolumeQuerySlices(const uint64_t* slices, size_t slices_count,
                                        fuchsia_hardware_block_volume_VsliceRange* out_ranges,
                                        size_t* out_ranges_count) const = 0;
  virtual zx_status_t VolumeExtend(uint64_t offset, uint64_t length) = 0;
  virtual zx_status_t VolumeShrink(uint64_t offset, uint64_t length) = 0;
};

}  // namespace block_client

#endif  // SRC_LIB_STORAGE_BLOCK_CLIENT_CPP_BLOCK_DEVICE_H_
