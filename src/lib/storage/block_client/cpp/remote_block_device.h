// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_STORAGE_BLOCK_CLIENT_CPP_REMOTE_BLOCK_DEVICE_H_
#define SRC_LIB_STORAGE_BLOCK_CLIENT_CPP_REMOTE_BLOCK_DEVICE_H_

#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <lib/zx/channel.h>
#include <lib/zx/fifo.h>
#include <lib/zx/vmo.h>

#include <memory>

#include "src/lib/storage/block_client/cpp/block_device.h"

namespace block_client {

// A concrete implementation of |BlockDevice|.
//
// This class is not movable or copyable.
class RemoteBlockDevice final : public BlockDevice {
 public:
  static zx_status_t Create(zx::channel device, std::unique_ptr<RemoteBlockDevice>* out);
  RemoteBlockDevice& operator=(RemoteBlockDevice&&) = delete;
  RemoteBlockDevice(RemoteBlockDevice&&) = delete;
  RemoteBlockDevice& operator=(const RemoteBlockDevice&) = delete;
  RemoteBlockDevice(const RemoteBlockDevice&) = delete;
  ~RemoteBlockDevice();

  zx_status_t ReadBlock(uint64_t block_num, uint64_t block_size, void* block) const final;
  zx_status_t FifoTransaction(block_fifo_request_t* requests, size_t count) final;
  zx::status<std::string> GetDevicePath() const final;
  zx_status_t BlockGetInfo(fuchsia_hardware_block_BlockInfo* out_info) const final;
  zx_status_t BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out_vmoid) final;
  zx_status_t VolumeGetInfo(fuchsia_hardware_block_volume_VolumeManagerInfo* out_manager_info,
                            fuchsia_hardware_block_volume_VolumeInfo* out_volume_info) const final;
  zx_status_t VolumeQuerySlices(const uint64_t* slices, size_t slices_count,
                                fuchsia_hardware_block_volume_VsliceRange* out_ranges,
                                size_t* out_ranges_count) const final;
  zx_status_t VolumeExtend(uint64_t offset, uint64_t length) final;
  zx_status_t VolumeShrink(uint64_t offset, uint64_t length) final;

 private:
  RemoteBlockDevice(zx::channel device, zx::fifo fifo);

  zx::channel device_;
  block_client::Client fifo_client_;
};

}  // namespace block_client

#endif  // SRC_LIB_STORAGE_BLOCK_CLIENT_CPP_REMOTE_BLOCK_DEVICE_H_
