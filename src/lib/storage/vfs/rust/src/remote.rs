// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A node which forwards open requests to a remote io.fidl server.

#[cfg(test)]
mod tests;

use crate::{
    common::send_on_open_with_error,
    directory::entry::{DirectoryEntry, EntryInfo},
    execution_scope::ExecutionScope,
    path::Path,
    service::connection::io1::Connection,
};

use {
    fidl::{self, endpoints::ServerEnd},
    fidl_fuchsia_io::{
        DirectoryProxy, NodeMarker, DIRENT_TYPE_DIRECTORY, DIRENT_TYPE_UNKNOWN, INO_UNKNOWN,
        OPEN_FLAG_NODE_REFERENCE, OPEN_FLAG_NO_REMOTE,
    },
    fuchsia_zircon::Status,
    std::sync::Arc,
};

/// The type for the callback function used to create new connections to the remote object. The
/// arguments mirror DirectoryEntry::open.
pub type RoutingFn =
    Box<dyn Fn(ExecutionScope, u32, u32, Path, ServerEnd<NodeMarker>) + Send + Sync>;

/// Create a new [`Remote`] node that forwards requests to the provided [`RoutingFn`]. This routing
/// function is called once per open request. The dirent type is set to the provided
/// `dirent_type`, which should be one of the `DIRENT_TYPE_*` values defined in fuchsia.io.
pub fn remote_boxed_with_type(open: RoutingFn, dirent_type: u8) -> Arc<Remote> {
    Arc::new(Remote { open, dirent_type })
}

/// Create a new [`Remote`] node that forwards open requests to the provided [`RoutingFn`]. This
/// routing function is called once per open request. The dirent type is set as
/// `DIRENT_TYPE_UNKNOWN`. If the remote node is a known `DIRENT_TYPE_*` type, you may wish to use
/// [`remote_boxed_with_type`] instead.
pub fn remote_boxed(open: RoutingFn) -> Arc<Remote> {
    remote_boxed_with_type(open, DIRENT_TYPE_UNKNOWN)
}

/// Create a new [`Remote`] node that forwards open requests to the provided callback. This routing
/// function is called once per open request. The dirent type is set as `DIRENT_TYPE_UNKNOWN`. If
/// the remote node is a known `DIRENT_TYPE_*` type, you may wish to use [`remote_boxed_with_type`]
/// instead.
pub fn remote<Open>(open: Open) -> Arc<Remote>
where
    Open: Fn(ExecutionScope, u32, u32, Path, ServerEnd<NodeMarker>) + Send + Sync + 'static,
{
    remote_boxed(Box::new(open))
}

/// Create a new [`Remote`] node that forwards open requests to the provided [`DirectoryProxy`],
/// effectively handing off the handling of any further requests to the remote fidl server.
pub fn remote_dir(dir: DirectoryProxy) -> Arc<Remote> {
    remote_boxed_with_type(
        Box::new(move |_scope, flags, mode, path, server_end| {
            if path.is_empty() {
                let _ = dir.open(flags, mode, ".", server_end);
            } else {
                let _ = dir.open(flags, mode, &path.into_string(), server_end);
            }
        }),
        DIRENT_TYPE_DIRECTORY,
    )
}

/// A Remote node is a node which forwards most open requests to another entity. The forwarding is
/// done by calling a routing function of type [`RoutingFn`] provided at the time of construction.
/// The remote node itself doesn't do any flag validation when forwarding the open call.
///
/// When a remote node is opened with OPEN_FLAG_NODE_REFERENCE or OPEN_FLAG_NO_REMOTE, a connection
/// to this local node is created instead.
pub struct Remote {
    open: RoutingFn,
    dirent_type: u8,
}

impl Remote {
    /// Open this node as a fuchsia.io/Node. This reuses the service connection implementation as
    /// they are functionally equivalent in this scenario.
    fn open_as_node(
        self: Arc<Self>,
        scope: ExecutionScope,
        flags: u32,
        mode: u32,
        server_end: ServerEnd<NodeMarker>,
    ) {
        Connection::create_connection(scope, flags, mode, server_end);
    }

    fn open_as_remote(
        self: Arc<Self>,
        scope: ExecutionScope,
        flags: u32,
        mode: u32,
        path: Path,
        server_end: ServerEnd<NodeMarker>,
    ) {
        // There is no flag validation to do here. All flags are either handled by the initial
        // connection that forwarded this open request (if it exists) or the remote node.
        (self.open)(scope, flags, mode, path, server_end);
    }
}

impl DirectoryEntry for Remote {
    fn open(
        self: Arc<Self>,
        scope: ExecutionScope,
        mut flags: u32,
        mode: u32,
        path: Path,
        server_end: ServerEnd<NodeMarker>,
    ) {
        if flags & OPEN_FLAG_NODE_REFERENCE != 0 || flags & OPEN_FLAG_NO_REMOTE != 0 {
            if !path.is_empty() {
                send_on_open_with_error(flags, server_end, Status::BAD_PATH);
                return;
            }
            // for the purposes of the rust vfs, NO_REMOTE is essentially the same as
            // NODE_REFERENCE.
            if flags & OPEN_FLAG_NO_REMOTE != 0 {
                flags &= !OPEN_FLAG_NO_REMOTE;
                flags |= OPEN_FLAG_NODE_REFERENCE;
            }
            self.open_as_node(scope, flags, mode, server_end);
        } else {
            self.open_as_remote(scope, flags, mode, path, server_end);
        }
    }

    fn entry_info(&self) -> EntryInfo {
        EntryInfo::new(INO_UNKNOWN, self.dirent_type)
    }

    fn can_hardlink(&self) -> bool {
        false
    }
}
