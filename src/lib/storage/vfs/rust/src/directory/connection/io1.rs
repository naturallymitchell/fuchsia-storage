// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    common::{inherit_rights_for_clone, send_on_open_with_error},
    directory::{
        common::{check_child_connection_flags, POSIX_DIRECTORY_PROTECTION_ATTRIBUTES},
        entry::DirectoryEntry,
        entry_container::{self, AsyncGetEntry, AsyncReadDirents, EntryContainer, Observable},
        read_dirents,
    },
    execution_scope::ExecutionScope,
    path::Path,
};

use {
    anyhow::Error,
    fidl::{endpoints::ServerEnd, Handle},
    fidl_fuchsia_io::{
        DirectoryCloseResponder, DirectoryControlHandle, DirectoryDescribeResponder,
        DirectoryGetAttrResponder, DirectoryGetTokenResponder, DirectoryLinkResponder,
        DirectoryNodeGetFlagsResponder, DirectoryNodeSetFlagsResponder, DirectoryObject,
        DirectoryReadDirentsResponder, DirectoryRenameResponder, DirectoryRequest,
        DirectoryRequestStream, DirectoryRewindResponder, DirectorySetAttrResponder,
        DirectorySyncResponder, DirectoryUnlinkResponder, DirectoryWatchResponder, NodeAttributes,
        NodeInfo, NodeMarker, INO_UNKNOWN, MODE_TYPE_DIRECTORY,
    },
    fuchsia_async::Channel,
    fuchsia_zircon::{
        sys::{ZX_ERR_INVALID_ARGS, ZX_ERR_NOT_SUPPORTED, ZX_OK},
        Status,
    },
    futures::{future::BoxFuture, stream::StreamExt},
    std::{default::Default, iter, iter::ExactSizeIterator, mem::replace, sync::Arc},
};

/// Return type for [`BaseConnection::handle_request`] and [`DerivedConnection::handle_request`].
pub enum ConnectionState {
    /// Connection is still alive.
    Alive,
    /// Connection have received Node::Close message and should be closed.
    Closed,
}

/// This is an API a derived directory connection needs to implement, in order for the
/// `BaseConnection` to be able to interact with it.
pub trait DerivedConnection<TraversalPosition>: Send + Sync
where
    TraversalPosition: Default + Send + Sync + 'static,
{
    type Directory: BaseConnectionClient<TraversalPosition> + ?Sized;

    fn new(scope: ExecutionScope, directory: Arc<Self::Directory>, flags: u32) -> Self;

    /// Initializes a directory connection, checking the flags and sending `OnOpen` event if
    /// necessary.  Then either runs this connection inside of the specified `scope` or, in case of
    /// an error, sends an appropriate `OnOpen` event (if requested) over the `server_end`
    /// connection.
    fn create_connection(
        scope: ExecutionScope,
        directory: Arc<Self::Directory>,
        flags: u32,
        mode: u32,
        server_end: ServerEnd<NodeMarker>,
    );

    fn entry_not_found(
        scope: ExecutionScope,
        parent: Arc<dyn DirectoryEntry>,
        flags: u32,
        mode: u32,
        name: &str,
        path: &Path,
    ) -> Result<Arc<dyn DirectoryEntry>, Status>;

    fn handle_request(
        &mut self,
        request: DirectoryRequest,
    ) -> BoxFuture<'_, Result<ConnectionState, Error>>;
}

/// This is an API a directory needs to implement, in order for the `BaseConnection` to be able to
/// interact with it.
pub trait BaseConnectionClient<TraversalPosition>:
    DirectoryEntry + EntryContainer + entry_container::Observable<TraversalPosition> + Send + Sync
where
    TraversalPosition: Default + Send + Sync + 'static,
{
}

impl<TraversalPosition, T> BaseConnectionClient<TraversalPosition> for T
where
    TraversalPosition: Default + Send + Sync + 'static,
    T: DirectoryEntry
        + EntryContainer
        + entry_container::Observable<TraversalPosition>
        + Send
        + Sync
        + 'static,
{
}

/// Handles functionality shared between mutable and immutable FIDL connections to a directory.  A
/// single directory may contain multiple connections.  Instances of the `BaseConnection`
/// will also hold any state that is "per-connection".  Currently that would be the access flags
/// and the seek position.
pub(in crate::directory) struct BaseConnection<Connection, TraversalPosition>
where
    Connection: DerivedConnection<TraversalPosition> + 'static,
    TraversalPosition: Default + Send + Sync + 'static,
{
    /// Execution scope this connection and any async operations and connections it creates will
    /// use.
    pub(in crate::directory) scope: ExecutionScope,

    pub(in crate::directory) directory: Arc<Connection::Directory>,

    /// Flags set on this connection when it was opened or cloned.
    pub(in crate::directory) flags: u32,

    /// Seek position for this connection to the directory.  We just store the element that was
    /// returned last by ReadDirents for this connection.  Next call will look for the next element
    /// in alphabetical order and resume from there.
    ///
    /// An alternative is to use an intrusive tree to have a dual index in both names and IDs that
    /// are assigned to the entries in insertion order.  Then we can store an ID instead of the
    /// full entry name.  This is what the C++ version is doing currently.
    ///
    /// It should be possible to do the same intrusive dual-indexing using, for example,
    ///
    ///     https://docs.rs/intrusive-collections/0.7.6/intrusive_collections/
    ///
    /// but, as, I think, at least for the pseudo directories, this approach is fine, and it simple
    /// enough.
    seek: TraversalPosition,
}

/// Subset of the [`DirectoryRequest`] protocol that is handled by the
/// [`BaseConnection::handle_request`] method.
pub(in crate::directory) enum BaseDirectoryRequest {
    Clone {
        flags: u32,
        object: ServerEnd<NodeMarker>,
        #[allow(unused)]
        control_handle: DirectoryControlHandle,
    },
    Close {
        responder: DirectoryCloseResponder,
    },
    Describe {
        responder: DirectoryDescribeResponder,
    },
    Sync {
        responder: DirectorySyncResponder,
    },
    GetAttr {
        responder: DirectoryGetAttrResponder,
    },
    SetAttr {
        #[allow(unused)]
        flags: u32,
        #[allow(unused)]
        attributes: NodeAttributes,
        responder: DirectorySetAttrResponder,
    },
    GetFlags {
        responder: DirectoryNodeGetFlagsResponder,
    },
    SetFlags {
        #[allow(unused)]
        flags: u32,
        responder: DirectoryNodeSetFlagsResponder,
    },
    Open {
        flags: u32,
        mode: u32,
        path: String,
        object: ServerEnd<NodeMarker>,
        #[allow(unused)]
        control_handle: DirectoryControlHandle,
    },
    ReadDirents {
        max_bytes: u64,
        responder: DirectoryReadDirentsResponder,
    },
    Rewind {
        responder: DirectoryRewindResponder,
    },
    Link {
        src: String,
        dst_parent_token: Handle,
        dst: String,
        responder: DirectoryLinkResponder,
    },
    Watch {
        mask: u32,
        options: u32,
        watcher: fidl::Channel,
        responder: DirectoryWatchResponder,
    },
}

pub(in crate::directory) enum DerivedDirectoryRequest {
    Unlink {
        path: String,
        responder: DirectoryUnlinkResponder,
    },
    GetToken {
        responder: DirectoryGetTokenResponder,
    },
    Rename {
        src: String,
        dst_parent_token: Handle,
        dst: String,
        responder: DirectoryRenameResponder,
    },
}

pub(in crate::directory) enum DirectoryRequestType {
    Base(BaseDirectoryRequest),
    Derived(DerivedDirectoryRequest),
}

impl From<DirectoryRequest> for DirectoryRequestType {
    fn from(request: DirectoryRequest) -> Self {
        use {BaseDirectoryRequest::*, DerivedDirectoryRequest::*, DirectoryRequestType::*};

        match request {
            DirectoryRequest::Clone { flags, object, control_handle } => {
                Base(Clone { flags, object, control_handle })
            }
            DirectoryRequest::Close { responder } => Base(Close { responder }),
            DirectoryRequest::Describe { responder } => Base(Describe { responder }),
            DirectoryRequest::Sync { responder } => Base(Sync { responder }),
            DirectoryRequest::GetAttr { responder } => Base(GetAttr { responder }),
            DirectoryRequest::SetAttr { flags, attributes, responder } => {
                Base(SetAttr { flags, attributes, responder })
            }
            DirectoryRequest::NodeGetFlags { responder } => Base(GetFlags { responder }),
            DirectoryRequest::NodeSetFlags { flags, responder } => {
                Base(SetFlags { flags, responder })
            }
            DirectoryRequest::Open { flags, mode, path, object, control_handle } => {
                Base(Open { flags, mode, path, object, control_handle })
            }
            DirectoryRequest::Unlink { path, responder } => Derived(Unlink { path, responder }),
            DirectoryRequest::ReadDirents { max_bytes, responder } => {
                Base(ReadDirents { max_bytes, responder })
            }
            DirectoryRequest::Rewind { responder } => Base(Rewind { responder }),
            DirectoryRequest::GetToken { responder } => Derived(GetToken { responder }),
            DirectoryRequest::Rename { src, dst_parent_token, dst, responder } => {
                Derived(Rename { src, dst_parent_token, dst, responder })
            }
            DirectoryRequest::Link { src, dst_parent_token, dst, responder } => {
                Base(Link { src, dst_parent_token, dst, responder })
            }
            DirectoryRequest::Watch { mask, options, watcher, responder } => {
                Base(Watch { mask, options, watcher, responder })
            }
        }
    }
}

#[must_use = "handle_requests() returns an async task that needs to be run"]
pub(in crate::directory) async fn handle_requests<Connection, TraversalPosition>(
    mut requests: DirectoryRequestStream,
    mut connection: Connection,
) where
    Connection: DerivedConnection<TraversalPosition>,
    TraversalPosition: Default + Send + Sync + 'static,
{
    while let Some(request_or_err) = requests.next().await {
        match request_or_err {
            Err(_) => {
                // FIDL level error, such as invalid message format and alike.  Close the
                // connection on any unexpected error.
                // TODO: Send an epitaph.
                break;
            }
            Ok(request) => match connection.handle_request(request).await {
                Ok(ConnectionState::Alive) => (),
                Ok(ConnectionState::Closed) => break,
                Err(_) => {
                    // Protocol level error.  Close the connection on any unexpected error.
                    // TODO: Send an epitaph.
                    break;
                }
            },
        }
    }
}

impl<Connection, TraversalPosition> BaseConnection<Connection, TraversalPosition>
where
    Connection: DerivedConnection<TraversalPosition>,
    TraversalPosition: Default + Send + Sync + 'static,
{
    /// Constructs an instance of `BaseConnection` - to be used by derived connections, when they
    /// need to create a nested `BaseConnection` "sub-object".  But when implementing
    /// `create_connection`, derived connections should use the [`create_connection`] call.
    pub(in crate::directory) fn new(
        scope: ExecutionScope,
        directory: Arc<Connection::Directory>,
        flags: u32,
    ) -> Self {
        BaseConnection { scope, directory, flags, seek: Default::default() }
    }

    /// Handle a [`DirectoryRequest`].  This function is responsible for handing all the basic
    /// directory operations.
    // TODO(fxb/37419): Remove default handling after methods landed.
    #[allow(unreachable_patterns)]
    pub(in crate::directory) async fn handle_request(
        &mut self,
        request: BaseDirectoryRequest,
    ) -> Result<ConnectionState, Error> {
        match request {
            BaseDirectoryRequest::Clone { flags, object, control_handle: _ } => {
                self.handle_clone(flags, 0, object);
            }
            BaseDirectoryRequest::Close { responder } => {
                responder.send(ZX_OK)?;
                return Ok(ConnectionState::Closed);
            }
            BaseDirectoryRequest::Describe { responder } => {
                let mut info = NodeInfo::Directory(DirectoryObject);
                responder.send(&mut info)?;
            }
            BaseDirectoryRequest::Sync { responder } => {
                responder.send(ZX_ERR_NOT_SUPPORTED)?;
            }
            BaseDirectoryRequest::GetAttr { responder } => {
                let mut attrs = NodeAttributes {
                    mode: MODE_TYPE_DIRECTORY | POSIX_DIRECTORY_PROTECTION_ATTRIBUTES,
                    id: INO_UNKNOWN,
                    content_size: 0,
                    storage_size: 0,
                    link_count: 1,
                    creation_time: 0,
                    modification_time: 0,
                };
                responder.send(ZX_OK, &mut attrs)?;
            }
            BaseDirectoryRequest::SetAttr { flags: _, attributes: _, responder } => {
                // According to zircon/system/fidl/fuchsia-io/io.fidl the only flag that might be
                // modified through this call is OPEN_FLAG_APPEND, and it is not supported by a
                // Simple directory.
                responder.send(ZX_ERR_NOT_SUPPORTED)?;
            }
            BaseDirectoryRequest::GetFlags { responder } => {
                responder.send(ZX_OK, self.flags)?;
            }
            BaseDirectoryRequest::SetFlags { flags: _, responder } => {
                responder.send(ZX_ERR_NOT_SUPPORTED)?;
            }
            BaseDirectoryRequest::Open { flags, mode, path, object, control_handle: _ } => {
                self.handle_open(flags, mode, path, object);
            }
            BaseDirectoryRequest::ReadDirents { max_bytes, responder } => {
                self.handle_read_dirents(max_bytes, |status, entries| {
                    responder.send(status.into_raw(), entries)
                })
                .await?;
            }
            BaseDirectoryRequest::Rewind { responder } => {
                self.seek = Default::default();
                responder.send(ZX_OK)?;
            }
            BaseDirectoryRequest::Link { src, dst_parent_token, dst, responder } => {
                self.handle_link(src, dst_parent_token, dst, |status| {
                    responder.send(status.into_raw())
                })
                .await?;
            }
            BaseDirectoryRequest::Watch { mask, options, watcher, responder } => {
                if options != 0 {
                    responder.send(ZX_ERR_INVALID_ARGS)?;
                } else {
                    let channel = Channel::from_channel(watcher)?;
                    self.handle_watch(mask, channel, |status| responder.send(status.into_raw()))?;
                }
            }
            _ => {}
        }
        Ok(ConnectionState::Alive)
    }

    fn handle_clone(&self, flags: u32, mode: u32, server_end: ServerEnd<NodeMarker>) {
        let flags = match inherit_rights_for_clone(self.flags, flags) {
            Ok(updated) => updated,
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        Connection::create_connection(
            self.scope.clone(),
            self.directory.clone(),
            flags,
            mode,
            server_end,
        );
    }

    fn handle_open(
        &self,
        flags: u32,
        mut mode: u32,
        path: String,
        server_end: ServerEnd<NodeMarker>,
    ) {
        if path == "/" || path == "" {
            send_on_open_with_error(flags, server_end, Status::BAD_PATH);
            return;
        }

        if path == "." || path == "./" {
            self.handle_clone(flags, mode, server_end);
            return;
        }

        let path = match Path::validate_and_split(path) {
            Ok(path) => path,
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        if path.is_dir() {
            mode |= MODE_TYPE_DIRECTORY;
        }

        let flags = match check_child_connection_flags(self.flags, flags) {
            Ok(updated) => updated,
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        // It is up to the open method to handle OPEN_FLAG_DESCRIBE from this point on.
        let directory = self.directory.clone();
        directory.open(self.scope.clone(), flags, mode, path, server_end);
    }

    async fn handle_read_dirents<R>(
        &mut self,
        max_bytes: u64,
        responder: R,
    ) -> Result<(), fidl::Error>
    where
        R: FnOnce(Status, &mut dyn ExactSizeIterator<Item = u8>) -> Result<(), fidl::Error>,
    {
        let res = {
            let directory = self.directory.clone();
            match directory.read_dirents(
                replace(&mut self.seek, Default::default()),
                read_dirents::Sink::<TraversalPosition>::new(max_bytes),
            ) {
                AsyncReadDirents::Immediate(res) => res,
                AsyncReadDirents::Future(fut) => fut.await,
            }
        };

        let done_or_err = match res {
            Ok(sealed) => sealed.open().downcast::<read_dirents::Done<TraversalPosition>>(),
            Err(status) => return responder(status, &mut iter::empty()),
        };

        match done_or_err {
            Ok(done) => {
                self.seek = done.pos;
                responder(done.status, &mut done.buf.into_iter())
            }
            Err(_) => {
                debug_assert!(
                    false,
                    "`read_dirents()` returned a `dirents_sink::Sealed` instance that is not \
                     an instance of the `read_dirents::Done`.  This is a bug in the \
                     `read_dirents()` implementation."
                );
                responder(Status::NOT_SUPPORTED, &mut iter::empty())
            }
        }
    }

    async fn handle_link<R>(
        &self,
        src: String,
        dst_parent_token: Handle,
        dst: String,
        responder: R,
    ) -> Result<(), fidl::Error>
    where
        R: FnOnce(Status) -> Result<(), fidl::Error>,
    {
        let token_registry = match self.scope.token_registry() {
            None => return responder(Status::NOT_SUPPORTED),
            Some(registry) => registry,
        };

        let res = {
            let directory = self.directory.clone();
            match directory.get_entry(src) {
                AsyncGetEntry::Immediate(res) => res,
                AsyncGetEntry::Future(fut) => fut.await,
            }
        };

        let entry = match res {
            Err(status) => return responder(status),
            Ok(entry) => entry,
        };

        if !entry.can_hardlink() {
            return responder(Status::NOT_FILE);
        }

        let dst_parent = match token_registry.get_container(dst_parent_token) {
            Err(status) => return responder(status),
            Ok(None) => return responder(Status::NOT_FOUND),
            Ok(Some(entry)) => entry,
        };

        match dst_parent.link(dst, entry) {
            Ok(()) => responder(Status::OK),
            Err(status) => responder(status),
        }
    }

    fn handle_watch<R>(
        &mut self,
        mask: u32,
        channel: Channel,
        responder: R,
    ) -> Result<(), fidl::Error>
    where
        R: FnOnce(Status) -> Result<(), fidl::Error>,
    {
        let directory = self.directory.clone();
        let status = directory.register_watcher(self.scope.clone(), mask, channel);
        responder(status)
    }
}
