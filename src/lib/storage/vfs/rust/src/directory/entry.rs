// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Common trait for all the directory entry objects.

#![warn(missing_docs)]

use crate::{common::IntoAny, execution_scope::ExecutionScope, path::Path};

use {
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{
        NodeMarker, DIRENT_TYPE_BLOCK_DEVICE, DIRENT_TYPE_DIRECTORY, DIRENT_TYPE_FILE,
        DIRENT_TYPE_SERVICE, DIRENT_TYPE_SOCKET, DIRENT_TYPE_UNKNOWN, INO_UNKNOWN,
    },
    std::{fmt, sync::Arc},
};

/// Information about a directory entry, used to populate ReadDirents() output.
/// The first element is the inode number, or INO_UNKNOWN (from fuchsia.io) if not set, and the second
/// element is one of the DIRENT_TYPE_* constants defined in the fuchsia.io.
#[derive(PartialEq, Eq, Clone)]
pub struct EntryInfo(u64, u8);

impl EntryInfo {
    /// Constructs a new directory entry information object.
    pub fn new(inode: u64, type_: u8) -> EntryInfo {
        match type_ {
            DIRENT_TYPE_UNKNOWN
            | DIRENT_TYPE_DIRECTORY
            | DIRENT_TYPE_BLOCK_DEVICE
            | DIRENT_TYPE_FILE
            | DIRENT_TYPE_SOCKET
            | DIRENT_TYPE_SERVICE => EntryInfo(inode, type_),
            _ => panic!("Unexpected directory entry type: {}", type_),
        }
    }

    /// Retrives the `inode` argument of the [`EntryInfo::new()`] constructor.
    pub fn inode(&self) -> u64 {
        self.0
    }

    /// Retrives the `type_` argument of the [`EntryInfo::new()`] constructor.
    pub fn type_(&self) -> u8 {
        self.1
    }
}

impl fmt::Debug for EntryInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let new_type_str;
        let type_str = match self.type_() {
            DIRENT_TYPE_UNKNOWN => "Unknown",
            DIRENT_TYPE_DIRECTORY => "Directory",
            DIRENT_TYPE_BLOCK_DEVICE => "BlockDevice",
            DIRENT_TYPE_FILE => "File",
            DIRENT_TYPE_SOCKET => "Socket",
            DIRENT_TYPE_SERVICE => "Service",
            new_type => {
                new_type_str = format!("Unexpected EntryInfo type ({})", new_type);
                &new_type_str
            }
        };
        if self.inode() == INO_UNKNOWN {
            write!(f, "{}(INO_UNKNOWN)", type_str)
        } else {
            write!(f, "{}({})", type_str, self.inode())
        }
    }
}

/// Pseudo directories contain items that implement this trait.  Pseudo directories refer to the
/// items they contain as `Arc<dyn DirectoryEntry>`.
pub trait DirectoryEntry: IntoAny + Sync + Send {
    /// Opens a connection to this item if the `path` is "." or a connection to an item inside this
    /// one otherwise.  `path` will not contain any "." or ".." components.
    ///
    /// `flags` holds one or more of the `OPEN_RIGHT_*`, `OPEN_FLAG_*` constants.  Processing of the
    /// `flags` value is specific to the item - in particular, the `OPEN_RIGHT_*` flags need to
    /// match the item capabilities.  See the fuchsia.io documentation for the precise semantics of
    /// the `mode` argument.  Some validation of the `flags` and `mode` fields will have taken place
    /// prior to this call; `flags` and `mode` will be consistent with each other.
    ///
    /// It is the responsibility of the implementation to strip POSIX flags if the path crosses
    /// a boundary that does not have the required permissions.
    ///
    /// It is the responsibility of the implementation to send an `OnOpen` even on the channel
    /// contained by `server_end` in case `OPEN_FLAG_STATUS` was present in `flags`, and to
    /// populate the `info` part of the event if `OPEN_FLAG_DESCRIBE` was set.  This also applies
    /// to the error cases.
    ///
    /// This method is called via either `Open` or `Clone` fuchsia.io methods.  This is deliberate
    /// that this method does not return any errors.  Any errors that occur during this process
    /// should be sent as an `OnOpen` event over the `server_end` connection and the connection is
    /// then closed.  No errors should ever affect the connection where `Open` or `Clone` were
    /// received.
    fn open(
        self: Arc<Self>,
        scope: ExecutionScope,
        flags: u32,
        mode: u32,
        path: Path,
        server_end: ServerEnd<NodeMarker>,
    );

    /// This method is used to populate ReadDirents() output.
    fn entry_info(&self) -> EntryInfo;
}
