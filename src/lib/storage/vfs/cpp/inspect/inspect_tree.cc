// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/vfs/cpp/inspect/inspect_tree.h"

namespace fs_inspect {

namespace {

inspect::LazyNodeCallbackFn CreateInfoNode(std::function<InfoData()> info_callback) {
  return [info_callback = std::move(info_callback)]() {
    inspect::Inspector insp;
    InfoData info = info_callback();
    detail::Attach(insp, info);
    return fpromise::make_ok_promise(insp);
  };
}

inspect::LazyNodeCallbackFn CreateUsageNode(std::function<UsageData()> usage_callback) {
  return [usage_callback = std::move(usage_callback)]() {
    inspect::Inspector insp;
    UsageData usage = usage_callback();
    detail::Attach(insp, usage);
    return fpromise::make_ok_promise(insp);
  };
}

inspect::LazyNodeCallbackFn CreateVolumeNode(std::function<VolumeData()> volume_callback) {
  return [volume_callback = std::move(volume_callback)]() {
    inspect::Inspector insp;
    VolumeData volume = volume_callback();
    detail::Attach(insp, volume);
    return fpromise::make_ok_promise(insp);
  };
}

}  // namespace

FilesystemNodes CreateTree(inspect::Node& root, NodeCallbacks node_callbacks) {
  ZX_ASSERT(node_callbacks.info_callback != nullptr);
  ZX_ASSERT(node_callbacks.usage_callback != nullptr);
  ZX_ASSERT(node_callbacks.volume_callback != nullptr);
  return {
      .info = root.CreateLazyNode(kInfoNodeName,
                                  CreateInfoNode(std::move(node_callbacks.info_callback))),
      .usage = root.CreateLazyNode(kUsageNodeName,
                                   CreateUsageNode(std::move(node_callbacks.usage_callback))),
      .volume = root.CreateLazyNode(kVolumeNodeName,
                                    CreateVolumeNode(std::move(node_callbacks.volume_callback))),
      .detail = node_callbacks.detail_node_callback == nullptr
                    ? inspect::LazyNode{}
                    : root.CreateLazyNode(kDetailNodeName,
                                          std::move(node_callbacks.detail_node_callback)),
  };
}

}  // namespace fs_inspect
