// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FS_MOUNT_CHANNEL_H_
#define FS_MOUNT_CHANNEL_H_

#include <fuchsia/io/llcpp/fidl.h>
#include <lib/zx/channel.h>
#include <zircon/types.h>

#include <utility>

namespace fs {

// MountChannel functions exactly the same as a channel, except that it
// intentionally destructs by sending a clean "shutdown" signal to the
// underlying filesystem. Up until the point that a remote handle is
// attached to a vnode, this wrapper guarantees not only that the
// underlying handle gets closed on error, but also that the sub-filesystem
// is released (which cleans up the underlying connection to the block
// device).
class MountChannel {
 public:
  constexpr MountChannel() = default;

  explicit MountChannel(fidl::ClientEnd<llcpp::fuchsia::io::Directory> channel)
      : client_end_(std::move(channel)) {}

  MountChannel(MountChannel&& other) : client_end_(std::move(other.client_end_)) {}

  ~MountChannel();

  fidl::ClientEnd<llcpp::fuchsia::io::Directory>& client_end() { return client_end_; }

 private:
  fidl::ClientEnd<llcpp::fuchsia::io::Directory> client_end_;
};

}  // namespace fs

#endif  // FS_MOUNT_CHANNEL_H_
