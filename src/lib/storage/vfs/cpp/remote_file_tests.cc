// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>

#include <zxtest/zxtest.h>

#include "src/lib/storage/vfs/cpp/remote_file.h"

namespace {

TEST(RemoteFile, ApiTest) {
  auto endpoints = fidl::CreateEndpoints<::llcpp::fuchsia::io::Directory>();
  ASSERT_EQ(ZX_OK, endpoints.status_value());

  auto unowned_client = endpoints->client.borrow();
  auto file = fbl::MakeRefCounted<fs::RemoteFile>(std::move(endpoints->client));

  // get attributes
  fs::VnodeAttributes attr;
  EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
  EXPECT_EQ(V_TYPE_FILE | V_IRUSR, attr.mode);
  EXPECT_EQ(1, attr.link_count);

  // get remote properties
  EXPECT_TRUE(file->IsRemote());
  EXPECT_EQ(unowned_client, file->GetRemote());

  // detaching the remote mount isn't allowed
  EXPECT_TRUE(!file->DetachRemote());
}

}  // namespace
