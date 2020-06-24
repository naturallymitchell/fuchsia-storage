// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Connection to a directory that can be modified by the client though a FIDL connection.

use crate::{
    common::send_on_open_with_error,
    directory::{
        common::new_connection_validate_flags,
        connection::io1::{
            handle_requests, BaseConnection, BaseConnectionClient, ConnectionState,
            DerivedConnection, DerivedDirectoryRequest, DirectoryRequestType,
        },
        entry::DirectoryEntry,
        entry_container::MutableDirectory,
        mutable::entry_constructor::NewEntryType,
    },
    execution_scope::ExecutionScope,
    path::Path,
    registry::TokenRegistryClient,
};

use {
    anyhow::Error,
    fidl::{endpoints::ServerEnd, Handle},
    fidl_fuchsia_io::{
        DirectoryMarker, DirectoryObject, DirectoryRequest, NodeInfo, NodeMarker, OPEN_FLAG_CREATE,
        OPEN_FLAG_DESCRIBE, OPEN_RIGHT_WRITABLE,
    },
    fuchsia_zircon::Status,
    futures::future::BoxFuture,
    std::sync::Arc,
};

/// This is an API a mutable directory needs to implement, in order for a `MutableConnection` to be
/// able to interact with this it.
pub trait MutableConnectionClient: BaseConnectionClient + MutableDirectory {
    fn into_base_connection_client(self: Arc<Self>) -> Arc<dyn BaseConnectionClient>;

    fn into_mutable_directory(self: Arc<Self>) -> Arc<dyn MutableDirectory>;

    fn into_token_registry_client(self: Arc<Self>) -> Arc<dyn TokenRegistryClient>;
}

impl<T> MutableConnectionClient for T
where
    T: BaseConnectionClient + MutableDirectory + 'static,
{
    fn into_base_connection_client(self: Arc<Self>) -> Arc<dyn BaseConnectionClient> {
        self as Arc<dyn BaseConnectionClient>
    }

    fn into_mutable_directory(self: Arc<Self>) -> Arc<dyn MutableDirectory> {
        self as Arc<dyn MutableDirectory>
    }

    fn into_token_registry_client(self: Arc<Self>) -> Arc<dyn TokenRegistryClient> {
        self as Arc<dyn TokenRegistryClient>
    }
}

pub struct MutableConnection {
    base: BaseConnection<Self>,
}

impl DerivedConnection for MutableConnection {
    type Directory = dyn MutableConnectionClient;

    fn new(scope: ExecutionScope, directory: Arc<Self::Directory>, flags: u32) -> Self {
        MutableConnection { base: BaseConnection::<Self>::new(scope, directory, flags) }
    }

    fn create_connection(
        scope: ExecutionScope,
        directory: Arc<Self::Directory>,
        flags: u32,
        mode: u32,
        server_end: ServerEnd<NodeMarker>,
    ) {
        let flags = match new_connection_validate_flags(flags, mode) {
            Ok(updated) => updated,
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        let (requests, control_handle) =
            match ServerEnd::<DirectoryMarker>::new(server_end.into_channel())
                .into_stream_and_control_handle()
            {
                Ok((requests, control_handle)) => (requests, control_handle),
                Err(_) => {
                    // As we report all errors on `server_end`, if we failed to send an error over
                    // this connection, there is nowhere to send the error tothe error to.
                    return;
                }
            };

        if flags & OPEN_FLAG_DESCRIBE != 0 {
            let mut info = NodeInfo::Directory(DirectoryObject);
            match control_handle.send_on_open_(Status::OK.into_raw(), Some(&mut info)) {
                Ok(()) => (),
                Err(_) => return,
            }
        }

        let connection = Self::new(scope.clone(), directory, flags);

        let task = handle_requests::<Self>(requests, connection);
        // If we failed to send the task to the executor, it is probably shut down or is in the
        // process of shutting down (this is the only error state currently).  So there is nothing
        // for us to do, but to ignore the request.  Connection will be closed when the connection
        // object is dropped.
        let _ = scope.spawn(Box::pin(task));
    }

    fn entry_not_found(
        scope: ExecutionScope,
        parent: Arc<dyn DirectoryEntry>,
        flags: u32,
        mode: u32,
        name: &str,
        path: &Path,
    ) -> Result<Arc<dyn DirectoryEntry>, Status> {
        if flags & OPEN_FLAG_CREATE == 0 {
            return Err(Status::NOT_FOUND);
        }

        let type_ = NewEntryType::from_flags_and_mode(flags, mode, path.is_dir())?;

        let entry_constructor = match scope.entry_constructor() {
            None => return Err(Status::NOT_SUPPORTED),
            Some(constructor) => constructor,
        };

        entry_constructor.create_entry(parent, type_, name, path)
    }

    fn handle_request(
        &mut self,
        request: DirectoryRequest,
    ) -> BoxFuture<'_, Result<ConnectionState, Error>> {
        Box::pin(async move {
            match request.into() {
                DirectoryRequestType::Base(request) => self.base.handle_request(request).await,
                DirectoryRequestType::Derived(request) => {
                    self.handle_derived_request(request).await
                }
            }
        })
    }
}

impl MutableConnection {
    async fn handle_derived_request(
        &mut self,
        request: DerivedDirectoryRequest,
    ) -> Result<ConnectionState, Error> {
        match request {
            DerivedDirectoryRequest::Unlink { path, responder } => {
                self.handle_unlink(path, |status| responder.send(status.into_raw()))?;
            }
            DerivedDirectoryRequest::GetToken { responder } => {
                self.handle_get_token(|status, token| responder.send(status.into_raw(), token))?;
            }
            DerivedDirectoryRequest::Rename { src, dst_parent_token, dst, responder } => {
                self.handle_rename(src, dst_parent_token, dst, |status| {
                    responder.send(status.into_raw())
                })?;
            }
        }
        Ok(ConnectionState::Alive)
    }

    fn handle_unlink<R>(&mut self, path: String, responder: R) -> Result<(), fidl::Error>
    where
        R: FnOnce(Status) -> Result<(), fidl::Error>,
    {
        if self.base.flags & OPEN_RIGHT_WRITABLE == 0 {
            return responder(Status::ACCESS_DENIED);
        }

        if path == "/" || path == "" || path == "." || path == "./" {
            return responder(Status::BAD_PATH);
        }

        let mut path = match Path::validate_and_split(path) {
            Ok(path) => path,
            Err(status) => return responder(status),
        };

        let entry_name = match path.next() {
            None => return responder(Status::BAD_PATH),
            Some(name) => name.to_string(),
        };

        // We do not support traversal for the `Unlink` operation for now.  It is non-trivial, as
        // we need to go from node to node and we do not store their type information.  One
        // solution is to add `unlink` to `DirectoryEntry`, similar to `open`.  But, unlike `open`
        // it requires the operation to stay on the stack, even when we are hitting a mount point,
        // as we need to return status over the same connection.  C++ verison of "memfs" does not
        // do traversal, so we are not supporting it here either.  At least for now.
        //
        // Sean (smklein@) and Yifei (yifeit@) both agree that it should be removed from the
        // io.fidl spec.
        if !path.is_empty() {
            return responder(Status::BAD_PATH);
        }

        match self.base.directory.clone().unlink(entry_name) {
            Ok(()) => responder(Status::OK),
            Err(status) => responder(status),
        }
    }

    fn handle_get_token<R>(&self, responder: R) -> Result<(), fidl::Error>
    where
        R: FnOnce(Status, Option<Handle>) -> Result<(), fidl::Error>,
    {
        if self.base.flags & OPEN_RIGHT_WRITABLE == 0 {
            return responder(Status::ACCESS_DENIED, None);
        }

        let token_registry = match self.base.scope.token_registry() {
            None => return responder(Status::NOT_SUPPORTED, None),
            Some(registry) => registry,
        };

        let token = {
            let dir_as_reg_client = self.base.directory.clone().into_token_registry_client();
            match token_registry.get_token(dir_as_reg_client) {
                Err(status) => return responder(status, None),
                Ok(token) => token,
            }
        };

        responder(Status::OK, Some(token))
    }

    fn handle_rename<R>(
        &self,
        src: String,
        dst_parent_token: Handle,
        dst: String,
        responder: R,
    ) -> Result<(), fidl::Error>
    where
        R: FnOnce(Status) -> Result<(), fidl::Error>,
    {
        if self.base.flags & OPEN_RIGHT_WRITABLE == 0 {
            return responder(Status::ACCESS_DENIED);
        }

        let token_registry = match self.base.scope.token_registry() {
            None => return responder(Status::NOT_SUPPORTED),
            Some(registry) => registry,
        };

        let dst_parent = match token_registry.get_container(dst_parent_token) {
            Err(status) => return responder(status),
            Ok(None) => return responder(Status::NOT_FOUND),
            Ok(Some(entry)) => entry,
        };

        match self.base.directory.clone().into_mutable_directory().get_filesystem().rename(
            self.base.directory.clone().into_mutable_directory().into_any(),
            src,
            dst_parent.into_mutable_directory().into_any(),
            dst,
        ) {
            Ok(()) => responder(Status::OK),
            Err(status) => responder(status),
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            directory::{
                dirents_sink,
                entry::{DirectoryEntry, EntryInfo},
                entry_container::*,
                traversal_position::AlphabeticalTraversal,
            },
            filesystem::{Filesystem, FilesystemRename},
            registry::token_registry,
        },
        fidl::Channel,
        fidl_fuchsia_io::{DirectoryProxy, DIRENT_TYPE_DIRECTORY, OPEN_RIGHT_READABLE},
        fuchsia_async as fasync,
        std::{any::Any, sync::Mutex},
    };

    #[derive(Debug, PartialEq)]
    enum MutableDirectoryAction {
        Link { id: u32, path: String },
        Unlink { id: u32, path: String },
        Rename { id: u32, src_name: String, dst_dir: Arc<MockDirectory>, dst_name: String },
    }

    #[derive(Debug)]
    struct MockDirectory {
        id: u32,
        env: Arc<TestEnv>,
    }

    impl MockDirectory {
        pub fn new(id: u32, env: Arc<TestEnv>) -> Arc<Self> {
            Arc::new(MockDirectory { id, env })
        }
    }

    impl PartialEq for MockDirectory {
        fn eq(&self, other: &Self) -> bool {
            self.id == other.id
        }
    }

    impl DirectoryEntry for MockDirectory {
        fn open(
            self: Arc<Self>,
            _scope: ExecutionScope,
            _flags: u32,
            _mode: u32,
            _path: Path,
            _server_end: ServerEnd<NodeMarker>,
        ) {
            panic!("Not implemented!");
        }

        fn entry_info(&self) -> EntryInfo {
            EntryInfo::new(0, DIRENT_TYPE_DIRECTORY)
        }

        fn can_hardlink(&self) -> bool {
            // So we can use "self" in Directory::get_entry()
            // and expect link() to succeed.
            true
        }
    }

    impl Directory for MockDirectory {
        fn get_entry(self: Arc<Self>, _name: String) -> AsyncGetEntry {
            AsyncGetEntry::Immediate(Ok(self))
        }

        fn read_dirents(
            self: Arc<Self>,
            _pos: AlphabeticalTraversal,
            _sink: Box<dyn dirents_sink::Sink>,
        ) -> AsyncReadDirents {
            panic!("Not implemented");
        }

        fn register_watcher(
            self: Arc<Self>,
            _scope: ExecutionScope,
            _mask: u32,
            _channel: fasync::Channel,
        ) -> Status {
            panic!("Not implemented");
        }

        fn unregister_watcher(self: Arc<Self>, _key: usize) {
            panic!("Not implemented");
        }
    }

    impl MutableDirectory for MockDirectory {
        fn link(&self, path: String, _entry: Arc<dyn DirectoryEntry>) -> Result<(), Status> {
            self.env.handle_event(MutableDirectoryAction::Link { id: self.id, path })
        }

        fn unlink(&self, path: String) -> Result<(), Status> {
            self.env.handle_event(MutableDirectoryAction::Unlink { id: self.id, path })
        }

        fn get_filesystem(&self) -> Arc<dyn Filesystem> {
            Arc::new(MockFilesystem { env: self.env.clone() })
        }

        fn into_any(self: Arc<Self>) -> Arc<dyn Any + Send + Sync> {
            self as Arc<dyn Any + Send + Sync>
        }
    }

    struct MockFilesystem {
        env: Arc<TestEnv>,
    }

    impl FilesystemRename for MockFilesystem {
        fn rename(
            &self,
            src_dir: Arc<Any + Sync + Send + 'static>,
            src_name: String,
            dst_dir: Arc<Any + Sync + Send + 'static>,
            dst_name: String,
        ) -> Result<(), Status> {
            let src_dir = src_dir.downcast::<MockDirectory>().unwrap();
            let dst_dir = dst_dir.downcast::<MockDirectory>().unwrap();
            self.env.handle_event(MutableDirectoryAction::Rename {
                id: src_dir.id,
                src_name,
                dst_dir,
                dst_name,
            })
        }
    }

    impl Filesystem for MockFilesystem {}

    struct TestEnv {
        cur_id: Mutex<u32>,
        token_registry: Arc<token_registry::Simple>,
        events: Mutex<Vec<MutableDirectoryAction>>,
    }

    impl std::fmt::Debug for TestEnv {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.debug_struct("TestEnv").field("cur_id", &self.cur_id).finish()
        }
    }

    impl TestEnv {
        pub fn new() -> Arc<Self> {
            let token_registry = token_registry::Simple::new();
            Arc::new(TestEnv { cur_id: Mutex::new(0), token_registry, events: Mutex::new(vec![]) })
        }

        pub fn handle_event(&self, event: MutableDirectoryAction) -> Result<(), Status> {
            self.events.lock().unwrap().push(event);
            Ok(())
        }

        pub fn make_connection(
            self: Arc<Self>,
            flags: u32,
        ) -> (Arc<MockDirectory>, DirectoryProxy) {
            let scope = ExecutionScope::build(Box::new(fasync::EHandle::local()))
                .token_registry(self.token_registry.clone())
                .new();
            let mut cur_id = self.cur_id.lock().unwrap();
            let dir = MockDirectory::new(*cur_id, self.clone());
            *cur_id += 1;
            let (proxy, server_end) = fidl::endpoints::create_proxy::<DirectoryMarker>().unwrap();
            MutableConnection::create_connection(
                scope,
                dir.clone(),
                flags,
                0,
                server_end.into_channel().into(),
            );
            (dir, proxy)
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_rename() {
        let env = TestEnv::new();

        let (_dir, proxy) = env.clone().make_connection(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE);
        let (dir2, proxy2) = env.clone().make_connection(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE);

        let (status, token) = proxy2.get_token().await.unwrap();
        assert_eq!(Status::from_raw(status), Status::OK);

        let status = proxy.rename("src", token.unwrap(), "dest").await.unwrap();
        assert_eq!(Status::from_raw(status), Status::OK);

        let events = env.events.lock().unwrap();
        assert_eq!(
            *events,
            vec![MutableDirectoryAction::Rename {
                id: 0,
                src_name: "src".to_owned(),
                dst_dir: dir2,
                dst_name: "dest".to_owned(),
            },]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_link() {
        let env = TestEnv::new();
        let (_dir, proxy) = env.clone().make_connection(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE);
        let (_dir2, proxy2) =
            env.clone().make_connection(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE);

        let (status, token) = proxy2.get_token().await.unwrap();
        assert_eq!(Status::from_raw(status), Status::OK);

        let status = proxy.link("src", token.unwrap(), "dest").await.unwrap();
        assert_eq!(Status::from_raw(status), Status::OK);
        let events = env.events.lock().unwrap();
        assert_eq!(*events, vec![MutableDirectoryAction::Link { id: 1, path: "dest".to_owned() },]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_unlink() {
        let env = TestEnv::new();
        let (_dir, proxy) = env.clone().make_connection(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE);
        let status = proxy.unlink("test").await.unwrap();
        assert_eq!(Status::from_raw(status), Status::OK);
        let events = env.events.lock().unwrap();
        assert_eq!(
            *events,
            vec![MutableDirectoryAction::Unlink { id: 0, path: "test".to_owned() },]
        );
    }
}
