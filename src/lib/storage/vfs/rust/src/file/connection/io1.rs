// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        common::{inherit_rights_for_clone, send_on_open_with_error, GET_FLAGS_VISIBLE},
        directory::entry::DirectoryEntry,
        execution_scope::ExecutionScope,
        file::{
            common::{get_buffer_validate_flags, new_connection_validate_flags},
            connection::util::OpenFile,
            File,
        },
        path::Path,
    },
    anyhow::Error,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{
        FileMarker, FileRequest, FileRequestStream, NodeAttributes, NodeMarker, SeekOrigin,
        VmoFlags, INO_UNKNOWN, OPEN_FLAG_APPEND, OPEN_FLAG_DESCRIBE, OPEN_FLAG_NODE_REFERENCE,
        OPEN_FLAG_TRUNCATE, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE,
    },
    fuchsia_zircon::{
        self as zx,
        sys::{ZX_ERR_NOT_SUPPORTED, ZX_OK},
    },
    futures::{channel::oneshot, select, stream::StreamExt},
    static_assertions::assert_eq_size,
    std::sync::Arc,
};

/// Represents a FIDL connection to a file.
pub struct FileConnection<T: 'static + File> {
    /// Execution scope this connection and any async operations and connections it creates will
    /// use.
    scope: ExecutionScope,

    /// File this connection is associated with.
    file: OpenFile<T>,

    /// Wraps a FIDL connection, providing messages coming from the client.
    requests: FileRequestStream,

    /// Either the "flags" value passed into [`DirectoryEntry::open()`], or the "flags" value
    /// received with [`value@FileRequest::Clone`].
    flags: u32,

    /// Seek position. Next byte to be read or written within the buffer. This might be beyond the
    /// current size of buffer, matching POSIX:
    ///
    ///     http://pubs.opengroup.org/onlinepubs/9699919799/functions/lseek.html
    ///
    /// It will cause the buffer to be extended with zeroes (if necessary) when write() is called.
    // While the content in the buffer vector uses usize for the size, it is easier to use u64 to
    // match the FIDL bindings API. Pseudo files are not expected to cross the 2^64 bytes size
    // limit. And all the code is much simpler when we just assume that usize is the same as u64.
    // Should we need to port to a 128 bit platform, there are static assertions in the code that
    // would fail.
    seek: u64,
}

/// Return type for [`handle_request()`] functions.
enum ConnectionState {
    /// Connection is still alive.
    Alive,
    /// Connection have received Node::Close message and the [`handle_close`] method has been
    /// already called for this connection.
    Closed,
    /// Connection has been dropped by the peer or an error has occurred.  [`handle_close`] still
    /// need to be called (though it would not be able to report the status to the peer).
    Dropped,
}

impl<T: 'static + File> FileConnection<T> {
    /// Initialized a file connection, which will be running in the context of the specified
    /// execution `scope`.  This function will also check the flags and will send the `OnOpen`
    /// event if necessary.
    ///
    /// Per connection buffer is initialized using the `init_buffer` closure, as part of the
    /// connection initialization.
    pub fn create_connection(
        scope: ExecutionScope,
        file: Arc<T>,
        flags: u32,
        server_end: ServerEnd<NodeMarker>,
        readable: bool,
        writable: bool,
        executable: bool,
    ) {
        // If we failed to send the task to the executor, it is probably shut down or is in the
        // process of shutting down (this is the only error state currently). `server_end` and the
        // file will be closed when they're dropped - there seems to be no error to report there.
        let _ = scope.clone().spawn_with_shutdown(move |shutdown| {
            Self::create_connection_async(
                scope, file, flags, server_end, readable, writable, executable, shutdown,
            )
        });
    }

    /// Same as create_connection, but does not spawn a new task.
    pub async fn create_connection_async(
        scope: ExecutionScope,
        file: Arc<T>,
        flags: u32,
        server_end: ServerEnd<NodeMarker>,
        readable: bool,
        writable: bool,
        executable: bool,
        shutdown: oneshot::Receiver<()>,
    ) {
        // RAII helper that ensures that the file is closed if we fail to create the connection.
        let file = OpenFile::new(file, scope.clone());

        let flags = match new_connection_validate_flags(
            flags, readable, writable, executable, /*append_allowed=*/ true,
        ) {
            Ok(updated) => updated,
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        match File::open(file.as_ref(), flags).await {
            Ok(()) => (),
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        if flags & OPEN_FLAG_TRUNCATE != 0 {
            if let Err(status) = file.truncate(0).await {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        }

        let info = if flags & OPEN_FLAG_DESCRIBE != 0 {
            match file.describe(flags) {
                Ok(info) => Some(info),
                Err(status) => {
                    send_on_open_with_error(flags, server_end, status);
                    return;
                }
            }
        } else {
            None
        };

        let (requests, control_handle) =
            match ServerEnd::<FileMarker>::new(server_end.into_channel())
                .into_stream_and_control_handle()
            {
                Ok((requests, control_handle)) => (requests, control_handle),
                Err(_) => {
                    // As we report all errors on `server_end`, if we failed to send an error over
                    // this connection, there is nowhere to send the error to.
                    return;
                }
            };

        if let Some(mut info) = info {
            match control_handle.send_on_open_(zx::Status::OK.into_raw(), Some(&mut info)) {
                Ok(()) => (),
                Err(_) => return,
            }
        }

        FileConnection { scope: scope.clone(), file, requests, flags, seek: 0 }
            .handle_requests(shutdown)
            .await;
    }

    async fn handle_requests(mut self, mut shutdown: oneshot::Receiver<()>) {
        loop {
            let request = select! {
                request = self.requests.next() => {
                    if let Some(request) = request {
                        request
                    } else {
                        return;
                    }
                },
                _ = shutdown => return,
            };

            let state = match request {
                Err(_) => {
                    // FIDL level error, such as invalid message format and alike.  Close the
                    // connection on any unexpected error.
                    // TODO: Send an epitaph.
                    ConnectionState::Dropped
                }
                Ok(request) => {
                    self.handle_request(request)
                        .await
                        // Protocol level error.  Close the connection on any unexpected error.
                        // TODO: Send an epitaph.
                        .unwrap_or(ConnectionState::Dropped)
                }
            };

            match state {
                ConnectionState::Alive => (),
                ConnectionState::Closed => break,
                ConnectionState::Dropped => break,
            }
        }

        // If the file is still open at this point, it will get closed when the OpenFile is
        // dropped.
    }

    /// Handle a [`FileRequest`]. This function is responsible for handing all the file operations
    /// that operate on the connection-specific buffer.
    async fn handle_request(&mut self, req: FileRequest) -> Result<ConnectionState, Error> {
        match req {
            FileRequest::Clone { flags, object, control_handle: _ } => {
                fuchsia_trace::duration!("storage", "File::Clone");
                self.handle_clone(self.flags, flags, object);
            }
            FileRequest::CloseDeprecated { responder } => {
                fuchsia_trace::duration!("storage", "File::CloseDeprecated");
                let status = self.file.close().await.err().unwrap_or(zx::Status::OK);
                // We are going to close the connection anyways, so there is no way to handle this
                // error.  TODO We may want to send it in an epitaph.
                let _ = responder.send(status.into_raw());
                return Ok(ConnectionState::Closed);
            }
            FileRequest::Close { responder } => {
                fuchsia_trace::duration!("storage", "File::Close");
                responder.send(&mut self.file.close().await.map_err(|status| status.into_raw()))?;
                return Ok(ConnectionState::Closed);
            }
            FileRequest::Describe { responder } => {
                fuchsia_trace::duration!("storage", "File::Describe");
                responder.send(&mut self.file.describe(self.flags)?)?;
            }
            FileRequest::SyncDeprecated { responder } => {
                fuchsia_trace::duration!("storage", "File::SyncDeprecated");
                let status = self.file.sync().await.err().unwrap_or(zx::Status::OK);
                responder.send(status.into_raw())?;
            }
            FileRequest::Sync { responder } => {
                fuchsia_trace::duration!("storage", "File::Sync");
                responder.send(&mut self.file.sync().await.map_err(|status| status.into_raw()))?;
            }
            FileRequest::GetAttr { responder } => {
                fuchsia_trace::duration!("storage", "File::GetAttr");
                let (status, mut attrs) = self.handle_get_attr().await;
                responder.send(status.into_raw(), &mut attrs)?;
            }
            FileRequest::SetAttr { flags, attributes, responder } => {
                fuchsia_trace::duration!("storage", "File::SetAttr");
                let status = self.handle_set_attr(flags, attributes).await;
                responder.send(status.into_raw())?;
            }
            FileRequest::ReadDeprecated { count, responder } => {
                fuchsia_trace::duration!("storage", "File::Read", "bytes" => count);
                let advance = match self.handle_read_at(self.seek, count).await {
                    Ok((buffer, bytes_read)) => {
                        responder
                            .send(zx::Status::OK.into_raw(), &buffer[..bytes_read as usize])?;
                        bytes_read
                    }
                    Err(status) => {
                        responder.send(status.into_raw(), &[0u8; 0])?;
                        0u64
                    }
                };
                self.seek += advance;
            }
            FileRequest::Read { count, responder } => {
                fuchsia_trace::duration!("storage", "File::Read", "bytes" => count);
                let advance = match self.handle_read_at(self.seek, count).await {
                    Ok((buffer, bytes_read)) => {
                        responder.send(&mut Ok(buffer[..bytes_read as usize].to_vec()))?;
                        bytes_read
                    }
                    Err(status) => {
                        responder.send(&mut Err(status.into_raw()))?;
                        0u64
                    }
                };
                self.seek += advance;
            }
            FileRequest::ReadAtDeprecated { offset, count, responder } => {
                fuchsia_trace::duration!(
                    "storage",
                    "File::ReadAtDeprecated",
                    "offset" => offset,
                    "bytes" => count
                );
                match self.handle_read_at(offset, count).await {
                    Ok((buffer, bytes_read)) => {
                        responder.send(zx::Status::OK.into_raw(), &buffer[..bytes_read as usize])?
                    }
                    Err(status) => responder.send(status.into_raw(), &[0u8; 0])?,
                }
            }
            FileRequest::ReadAt { offset, count, responder } => {
                fuchsia_trace::duration!(
                    "storage",
                    "File::ReadAt",
                    "offset" => offset,
                    "bytes" => count
                );
                match self.handle_read_at(offset, count).await {
                    Ok((buffer, bytes_read)) => {
                        responder.send(&mut Ok(buffer[..bytes_read as usize].to_vec()))?
                    }
                    Err(status) => responder.send(&mut Err(status.into_raw()))?,
                }
            }
            FileRequest::WriteDeprecated { data, responder } => {
                fuchsia_trace::duration!("storage", "File::WriteDeprecated", "bytes" => data.len() as u64);
                let (status, actual) = self.handle_write(&data).await;
                responder.send(status.into_raw(), actual)?;
            }
            FileRequest::Write { data, responder } => {
                fuchsia_trace::duration!("storage", "File::Write", "bytes" => data.len() as u64);
                let (status, actual) = self.handle_write(&data).await;
                if status == zx::Status::OK {
                    responder.send(&mut Ok(actual))?;
                } else {
                    responder.send(&mut Err(status.into_raw()))?;
                }
            }
            FileRequest::WriteAtDeprecated { offset, data, responder } => {
                fuchsia_trace::duration!(
                    "storage",
                    "File::WriteAtDeprecated",
                    "offset" => offset,
                    "bytes" => data.len() as u64
                );
                let (status, actual) = self.handle_write_at(offset, &data).await;
                responder.send(status.into_raw(), actual)?;
            }
            FileRequest::WriteAt { offset, data, responder } => {
                fuchsia_trace::duration!(
                    "storage",
                    "File::WriteAt",
                    "offset" => offset,
                    "bytes" => data.len() as u64
                );
                let (status, actual) = self.handle_write_at(offset, &data).await;
                if status == zx::Status::OK {
                    responder.send(&mut Ok(actual))?;
                } else {
                    responder.send(&mut Err(status.into_raw()))?;
                }
            }
            FileRequest::SeekDeprecated { offset, start, responder } => {
                fuchsia_trace::duration!("storage", "File::SeekDeprecated");
                let (status, seek) = self.handle_seek(offset, start).await;
                responder.send(status.into_raw(), seek)?;
            }
            FileRequest::Seek { origin, offset, responder } => {
                fuchsia_trace::duration!("storage", "File::Seek");
                let (status, seek) = self.handle_seek(offset, origin).await;
                if status == zx::Status::OK {
                    responder.send(&mut Ok(seek))?;
                } else {
                    responder.send(&mut Err(status.into_raw()))?;
                }
            }
            FileRequest::Truncate { length, responder } => {
                fuchsia_trace::duration!("storage", "File::Truncate", "length" => length);
                let status = self.handle_truncate(length).await;
                responder.send(status.into_raw())?;
            }
            FileRequest::Resize { length, responder } => {
                fuchsia_trace::duration!("storage", "File::Resize", "length" => length);
                let status = self.handle_truncate(length).await;
                if status == zx::Status::OK {
                    responder.send(&mut Ok(()))?;
                } else {
                    responder.send(&mut Err(status.into_raw()))?;
                }
            }
            FileRequest::GetFlags { responder } => {
                fuchsia_trace::duration!("storage", "File::GetFlags");
                responder.send(ZX_OK, self.flags & GET_FLAGS_VISIBLE)?;
            }
            FileRequest::GetFlagsDeprecatedUseNode { responder } => {
                fuchsia_trace::duration!("storage", "File::GetFlagsDeprecatedUseNode");
                responder.send(ZX_OK, self.flags & GET_FLAGS_VISIBLE)?;
            }
            FileRequest::SetFlags { flags, responder } => {
                fuchsia_trace::duration!("storage", "File::SetFlags");
                self.flags = (self.flags & !OPEN_FLAG_APPEND) | (flags & OPEN_FLAG_APPEND);
                responder.send(ZX_OK)?;
            }
            FileRequest::SetFlagsDeprecatedUseNode { flags, responder } => {
                fuchsia_trace::duration!("storage", "File::SetFlagsDeprecatedUseNode");
                self.flags = (self.flags & !OPEN_FLAG_APPEND) | (flags & OPEN_FLAG_APPEND);
                responder.send(ZX_OK)?;
            }
            FileRequest::GetBuffer { flags, responder } => {
                fuchsia_trace::duration!("storage", "File::GetBuffer");
                let (status, mut buffer) =
                    match self.handle_get_buffer(VmoFlags::from_bits_truncate(flags)).await {
                        Ok(buffer) => (zx::Status::OK, Some(buffer)),
                        Err(status) => (status, None),
                    };
                responder.send(status.into_raw(), buffer.as_mut())?;
            }
            FileRequest::GetBackingMemory { flags, responder } => {
                fuchsia_trace::duration!("storage", "File::GetBackingMemory");
                match self.handle_get_buffer(flags).await {
                    Ok(buffer) => {
                        responder.send(&mut Ok(buffer.vmo))?;
                    }
                    Err(status) => {
                        responder.send(&mut Err(status.into_raw()))?;
                    }
                }
            }
            FileRequest::AdvisoryLock { request: _, responder } => {
                fuchsia_trace::duration!("storage", "File::AdvisoryLock");
                responder.send(&mut Err(ZX_ERR_NOT_SUPPORTED))?;
            }
            FileRequest::QueryFilesystem { responder } => {
                fuchsia_trace::duration!("storage", "Directory::QueryFilesystem");
                match self.file.query_filesystem() {
                    Err(status) => responder.send(status.into_raw(), None)?,
                    Ok(mut info) => responder.send(0, Some(&mut info))?,
                }
            }
            // TODO(https://fxbug.dev/77623): Remove when the io1 -> io2 transition is complete.
            _ => panic!("Unhandled request!"),
        }
        Ok(ConnectionState::Alive)
    }

    fn handle_clone(&mut self, parent_flags: u32, flags: u32, server_end: ServerEnd<NodeMarker>) {
        let flags = match inherit_rights_for_clone(parent_flags, flags) {
            Ok(updated) => updated,
            Err(status) => {
                send_on_open_with_error(flags, server_end, status);
                return;
            }
        };

        let file: Arc<dyn DirectoryEntry> = self.file.clone();
        file.open(self.scope.clone(), flags, 0, Path::dot(), server_end);
    }

    async fn handle_get_attr(&mut self) -> (zx::Status, NodeAttributes) {
        let attributes = match self.file.get_attrs().await {
            Ok(attr) => attr,
            Err(status) => {
                return (
                    status,
                    NodeAttributes {
                        mode: 0,
                        id: INO_UNKNOWN,
                        content_size: 0,
                        storage_size: 0,
                        link_count: 0,
                        creation_time: 0,
                        modification_time: 0,
                    },
                )
            }
        };
        (zx::Status::OK, attributes)
    }

    async fn handle_read_at(
        &mut self,
        offset: u64,
        count: u64,
    ) -> Result<(Vec<u8>, u64), zx::Status> {
        if self.flags & OPEN_RIGHT_READABLE == 0 {
            return Err(zx::Status::BAD_HANDLE);
        }

        if count > fidl_fuchsia_io::MAX_BUF {
            return Err(zx::Status::OUT_OF_RANGE);
        }

        let mut buffer = vec![0u8; count as usize];
        self.file.read_at(offset, &mut buffer[..]).await.map(|count| (buffer, count))
    }

    async fn handle_write(&mut self, content: &[u8]) -> (zx::Status, u64) {
        if self.flags & OPEN_RIGHT_WRITABLE == 0 {
            return (zx::Status::BAD_HANDLE, 0);
        }

        if self.flags & OPEN_FLAG_APPEND != 0 {
            match self.file.append(content).await {
                Ok((bytes, offset)) => {
                    self.seek = offset;
                    (zx::Status::OK, bytes)
                }
                Err(e) => (e, 0),
            }
        } else {
            let (status, actual) = self.handle_write_at(self.seek, content).await;
            assert_eq_size!(usize, u64);
            self.seek += actual;
            (status, actual)
        }
    }

    async fn handle_write_at(&mut self, offset: u64, content: &[u8]) -> (zx::Status, u64) {
        if self.flags & OPEN_RIGHT_WRITABLE == 0 {
            return (zx::Status::BAD_HANDLE, 0);
        }

        match self.file.write_at(offset, content).await {
            Ok(bytes) => (zx::Status::OK, bytes),
            Err(e) => (e, 0),
        }
    }

    /// Move seek position to byte `offset` relative to the origin specified by `start`.
    async fn handle_seek(&mut self, offset: i64, start: SeekOrigin) -> (zx::Status, u64) {
        if self.flags & OPEN_FLAG_NODE_REFERENCE != 0 {
            return (zx::Status::BAD_HANDLE, 0);
        }

        let (status, new_seek) = match start {
            SeekOrigin::Start => (zx::Status::OK, offset as i128),

            SeekOrigin::Current => {
                assert_eq_size!(usize, i64);
                (zx::Status::OK, self.seek as i128 + offset as i128)
            }

            SeekOrigin::End => {
                let size = self.file.get_size().await;
                assert_eq_size!(usize, i64, u64);
                match size {
                    Ok(size) => (zx::Status::OK, size as i128 + offset as i128),
                    Err(e) => (e, self.seek as i128),
                }
            }
        };

        if new_seek < 0 {
            // Can't seek to before the end of a file.
            return (zx::Status::OUT_OF_RANGE, self.seek);
        } else {
            self.seek = new_seek as u64;
            return (status, self.seek);
        }
    }

    async fn handle_set_attr(&mut self, flags: u32, attrs: NodeAttributes) -> zx::Status {
        if self.flags & OPEN_RIGHT_WRITABLE == 0 {
            return zx::Status::BAD_HANDLE;
        }

        match self.file.set_attrs(flags, attrs).await {
            Ok(()) => zx::Status::OK,
            Err(status) => status,
        }
    }

    async fn handle_truncate(&mut self, length: u64) -> zx::Status {
        if self.flags & OPEN_RIGHT_WRITABLE == 0 {
            return zx::Status::BAD_HANDLE;
        }

        match self.file.truncate(length).await {
            Ok(()) => zx::Status::OK,
            Err(status) => status,
        }
    }

    async fn handle_get_buffer(
        &mut self,
        flags: VmoFlags,
    ) -> Result<fidl_fuchsia_mem::Buffer, zx::Status> {
        get_buffer_validate_flags(flags, self.flags)?;
        // TODO(fxbug.dev/88358): Pass the VmoFlags type to get_buffer rather than the raw bits.
        self.file.get_buffer(flags.bits() as u32).await
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        assert_matches::assert_matches,
        async_trait::async_trait,
        fidl_fuchsia_io::{
            FileEvent, FileInfo, FileObject, FileProxy, NodeInfo, Representation,
            CLONE_FLAG_SAME_RIGHTS, MODE_TYPE_FILE, NODE_ATTRIBUTE_FLAG_CREATION_TIME,
            NODE_ATTRIBUTE_FLAG_MODIFICATION_TIME, VMO_FLAG_EXEC, VMO_FLAG_READ,
        },
        fuchsia_async as fasync, fuchsia_zircon as zx,
        futures::prelude::*,
        lazy_static::lazy_static,
        std::sync::Mutex,
    };

    #[derive(Debug, PartialEq)]
    enum FileOperation {
        Init { flags: u32 },
        ReadAt { offset: u64, count: u64 },
        WriteAt { offset: u64, content: Vec<u8> },
        Append { content: Vec<u8> },
        Truncate { length: u64 },
        GetBuffer { flags: u32 },
        GetSize,
        GetAttrs,
        SetAttrs { flags: u32, attrs: NodeAttributes },
        Close,
        Sync,
    }

    type MockCallbackType = Box<Fn(&FileOperation) -> zx::Status + Sync + Send>;
    /// A fake file that just tracks what calls `FileConnection` makes on it.
    struct MockFile {
        /// The list of operations that have been called.
        operations: Mutex<Vec<FileOperation>>,
        /// Callback used to determine how to respond to given operation.
        callback: MockCallbackType,
        /// Only used for get_size/get_attributes
        file_size: u64,
    }

    lazy_static! {
        pub static ref MOCK_FILE_SIZE: u64 = 256;
    }
    const MOCK_FILE_ID: u64 = 10;
    const MOCK_FILE_LINKS: u64 = 2;
    const MOCK_FILE_CREATION_TIME: u64 = 10;
    const MOCK_FILE_MODIFICATION_TIME: u64 = 100;
    impl MockFile {
        pub fn new(callback: MockCallbackType) -> Arc<Self> {
            Arc::new(MockFile {
                operations: Mutex::new(Vec::new()),
                callback,
                file_size: *MOCK_FILE_SIZE,
            })
        }

        fn handle_operation(&self, operation: FileOperation) -> Result<(), zx::Status> {
            let result = (self.callback)(&operation);
            self.operations.lock().unwrap().push(operation);
            match result {
                zx::Status::OK => Ok(()),
                err => Err(err),
            }
        }
    }

    #[async_trait]
    impl File for MockFile {
        async fn open(&self, flags: u32) -> Result<(), zx::Status> {
            self.handle_operation(FileOperation::Init { flags })?;
            Ok(())
        }

        async fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<u64, zx::Status> {
            let count = buffer.len() as u64;
            self.handle_operation(FileOperation::ReadAt { offset, count })?;

            // Return data as if we were a file with 0..255 repeated endlessly.
            let mut i = offset;
            buffer.fill_with(|| {
                let v = (i % 256) as u8;
                i += 1;
                v
            });
            Ok(count)
        }

        async fn write_at(&self, offset: u64, content: &[u8]) -> Result<u64, zx::Status> {
            self.handle_operation(FileOperation::WriteAt { offset, content: content.to_vec() })?;
            Ok(content.len() as u64)
        }

        async fn append(&self, content: &[u8]) -> Result<(u64, u64), zx::Status> {
            self.handle_operation(FileOperation::Append { content: content.to_vec() })?;
            Ok((content.len() as u64, self.file_size + content.len() as u64))
        }

        async fn truncate(&self, length: u64) -> Result<(), zx::Status> {
            self.handle_operation(FileOperation::Truncate { length })
        }

        async fn get_buffer(&self, flags: u32) -> Result<fidl_fuchsia_mem::Buffer, zx::Status> {
            self.handle_operation(FileOperation::GetBuffer { flags })?;
            Err(zx::Status::NOT_SUPPORTED)
        }

        async fn get_size(&self) -> Result<u64, zx::Status> {
            self.handle_operation(FileOperation::GetSize)?;
            Ok(self.file_size)
        }

        async fn get_attrs(&self) -> Result<NodeAttributes, zx::Status> {
            self.handle_operation(FileOperation::GetAttrs)?;
            Ok(NodeAttributes {
                mode: MODE_TYPE_FILE,
                id: MOCK_FILE_ID,
                content_size: self.file_size,
                storage_size: 2 * self.file_size,
                link_count: MOCK_FILE_LINKS,
                creation_time: MOCK_FILE_CREATION_TIME,
                modification_time: MOCK_FILE_MODIFICATION_TIME,
            })
        }

        async fn set_attrs(&self, flags: u32, attrs: NodeAttributes) -> Result<(), zx::Status> {
            self.handle_operation(FileOperation::SetAttrs { flags, attrs })?;
            Ok(())
        }

        async fn close(&self) -> Result<(), zx::Status> {
            self.handle_operation(FileOperation::Close)?;
            Ok(())
        }

        async fn sync(&self) -> Result<(), zx::Status> {
            self.handle_operation(FileOperation::Sync)
        }
    }

    impl DirectoryEntry for MockFile {
        fn open(
            self: Arc<Self>,
            scope: ExecutionScope,
            flags: u32,
            _mode: u32,
            path: Path,
            server_end: ServerEnd<NodeMarker>,
        ) {
            assert!(path.is_empty());

            FileConnection::create_connection(
                scope.clone(),
                self.clone(),
                flags,
                server_end.into_channel().into(),
                true,
                true,
                false,
            );
        }

        fn entry_info(&self) -> crate::directory::entry::EntryInfo {
            todo!();
        }
    }

    /// Only the init operation will succeed, all others fail.
    fn only_allow_init(op: &FileOperation) -> zx::Status {
        match op {
            FileOperation::Init { .. } => zx::Status::OK,
            _ => zx::Status::IO,
        }
    }

    /// All operations succeed.
    fn always_succeed_callback(_op: &FileOperation) -> zx::Status {
        zx::Status::OK
    }

    struct TestEnv {
        pub file: Arc<MockFile>,
        pub proxy: FileProxy,
        pub scope: ExecutionScope,
    }

    fn init_mock_file(callback: MockCallbackType, flags: u32) -> TestEnv {
        let file = MockFile::new(callback);
        let (proxy, server_end) =
            fidl::endpoints::create_proxy::<FileMarker>().expect("Create proxy to succeed");

        let scope = ExecutionScope::new();
        FileConnection::create_connection(
            scope.clone(),
            file.clone(),
            flags,
            server_end.into_channel().into(),
            true,
            true,
            false,
        );

        TestEnv { file, proxy, scope }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_open_flag_truncate() {
        let env = init_mock_file(
            Box::new(always_succeed_callback),
            OPEN_RIGHT_WRITABLE | OPEN_FLAG_TRUNCATE,
        );
        // Do a no-op sync() to make sure that the open has finished.
        let () = env.proxy.sync().await.unwrap().map_err(zx::Status::from_raw).unwrap();
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_WRITABLE | OPEN_FLAG_TRUNCATE },
                FileOperation::Truncate { length: 0 },
                FileOperation::Sync,
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_clone_same_rights() {
        let env = init_mock_file(
            Box::new(always_succeed_callback),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        );
        // Read from original proxy.
        let _: Vec<u8> = env.proxy.read(6).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        let (clone_proxy, remote) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        env.proxy.clone(CLONE_FLAG_SAME_RIGHTS, remote.into_channel().into()).unwrap();
        // Seek and read from clone_proxy.
        let _: u64 = clone_proxy
            .seek(SeekOrigin::Start, 100)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();
        let _: Vec<u8> = clone_proxy.read(5).await.unwrap().map_err(zx::Status::from_raw).unwrap();

        // Read from original proxy.
        let _: Vec<u8> = env.proxy.read(5).await.unwrap().map_err(zx::Status::from_raw).unwrap();

        let events = env.file.operations.lock().unwrap();
        // Each connection should have an independent seek.
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE },
                FileOperation::ReadAt { offset: 0, count: 6 },
                FileOperation::Init { flags: OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE },
                FileOperation::ReadAt { offset: 100, count: 5 },
                FileOperation::ReadAt { offset: 6, count: 5 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_close_succeeds() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let () = env.proxy.close().await.unwrap().map_err(zx::Status::from_raw).unwrap();

        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![FileOperation::Init { flags: OPEN_RIGHT_READABLE }, FileOperation::Close {},]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_close_fails() {
        let env = init_mock_file(Box::new(only_allow_init), OPEN_RIGHT_READABLE);
        let status = env.proxy.close().await.unwrap().map_err(zx::Status::from_raw);
        assert_eq!(status, Err(zx::Status::IO));

        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![FileOperation::Init { flags: OPEN_RIGHT_READABLE }, FileOperation::Close {},]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_close_called_when_dropped() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let _ = env.proxy.sync().await;
        std::mem::drop(env.proxy);
        env.scope.shutdown();
        env.scope.wait().await;
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::Sync,
                FileOperation::Close,
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_describe() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let info = env.proxy.describe().await.unwrap();
        match info {
            NodeInfo::File { .. } => (),
            _ => panic!("Expected NodeInfo::File, got {:?}", info),
        };
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_getattr() {
        let env = init_mock_file(Box::new(always_succeed_callback), 0);
        let (status, attributes) = env.proxy.get_attr().await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        assert_eq!(
            attributes,
            NodeAttributes {
                mode: MODE_TYPE_FILE,
                id: MOCK_FILE_ID,
                content_size: *MOCK_FILE_SIZE,
                storage_size: 2 * *MOCK_FILE_SIZE,
                link_count: MOCK_FILE_LINKS,
                creation_time: MOCK_FILE_CREATION_TIME,
                modification_time: MOCK_FILE_MODIFICATION_TIME,
            }
        );
        let events = env.file.operations.lock().unwrap();
        assert_eq!(*events, vec![FileOperation::Init { flags: 0 }, FileOperation::GetAttrs,]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_getbuffer() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let (status, _buffer) = env.proxy.get_buffer(VMO_FLAG_READ).await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::NOT_SUPPORTED);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::GetBuffer { flags: VMO_FLAG_READ },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_getbuffer_no_perms() {
        let env = init_mock_file(Box::new(always_succeed_callback), 0);
        let (status, buffer) = env.proxy.get_buffer(VMO_FLAG_READ).await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::ACCESS_DENIED);
        assert!(buffer.is_none());
        let events = env.file.operations.lock().unwrap();
        assert_eq!(*events, vec![FileOperation::Init { flags: 0 },]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_getbuffer_vmo_exec_requires_right_executable() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let (status, buffer) = env.proxy.get_buffer(VMO_FLAG_EXEC).await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::ACCESS_DENIED);
        assert!(buffer.is_none());
        let events = env.file.operations.lock().unwrap();
        assert_eq!(*events, vec![FileOperation::Init { flags: OPEN_RIGHT_READABLE },]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_getflags() {
        let env = init_mock_file(
            Box::new(always_succeed_callback),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_TRUNCATE,
        );
        let (status, flags) = env.proxy.get_flags().await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        // OPEN_FLAG_TRUNCATE should get stripped because it only applies at open time.
        assert_eq!(flags, OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init {
                    flags: OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_TRUNCATE
                },
                FileOperation::Truncate { length: 0 }
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_open_flag_describe() {
        let env = init_mock_file(
            Box::new(always_succeed_callback),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_DESCRIBE,
        );
        let event = env.proxy.take_event_stream().try_next().await.unwrap();
        match event {
            Some(FileEvent::OnOpen_ { s, info: Some(boxed) }) => {
                assert_eq!(zx::Status::from_raw(s), zx::Status::OK);
                assert_eq!(*boxed, NodeInfo::File(FileObject { event: None, stream: None }));
            }
            Some(FileEvent::OnConnectionInfo { info }) => {
                assert_eq!(info.representation, Some(Representation::File(FileInfo::EMPTY)));
            }
            e => panic!("Expected OnOpen event with NodeInfo::File, got {:?}", e),
        }
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![FileOperation::Init {
                flags: OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_DESCRIBE
            },]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_succeeds() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let data = env.proxy.read(10).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(data, vec![0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);

        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::ReadAt { offset: 0, count: 10 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_not_readable() {
        let env = init_mock_file(Box::new(only_allow_init), OPEN_RIGHT_WRITABLE);
        let result = env.proxy.read(10).await.unwrap().map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::BAD_HANDLE));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_validates_count() {
        let env = init_mock_file(Box::new(only_allow_init), OPEN_RIGHT_READABLE);
        let result = env
            .proxy
            .read(fidl_fuchsia_io::MAX_BUF + 1)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::OUT_OF_RANGE));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_at_succeeds() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let data = env.proxy.read_at(5, 10).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(data, vec![10, 11, 12, 13, 14]);

        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::ReadAt { offset: 10, count: 5 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_at_validates_count() {
        let env = init_mock_file(Box::new(only_allow_init), OPEN_RIGHT_READABLE);
        let result = env
            .proxy
            .read_at(fidl_fuchsia_io::MAX_BUF + 1, 0)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::OUT_OF_RANGE));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_seek_start() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let offset = env
            .proxy
            .seek(SeekOrigin::Start, 10)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();
        assert_eq!(offset, 10);

        let data = env.proxy.read(1).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(data, vec![10]);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::ReadAt { offset: 10, count: 1 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_seek_cur() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let offset = env
            .proxy
            .seek(SeekOrigin::Start, 10)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();
        assert_eq!(offset, 10);

        let offset = env
            .proxy
            .seek(SeekOrigin::Current, -2)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();
        assert_eq!(offset, 8);

        let data = env.proxy.read(1).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(data, vec![8]);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::ReadAt { offset: 8, count: 1 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_seek_before_start() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let result =
            env.proxy.seek(SeekOrigin::Current, -4).await.unwrap().map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::OUT_OF_RANGE));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_seek_end() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let offset = env
            .proxy
            .seek(SeekOrigin::End, -4)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();
        assert_eq!(offset, *MOCK_FILE_SIZE - 4);

        let data = env.proxy.read(1).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(data, vec![(offset % 256) as u8]);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_READABLE },
                FileOperation::GetSize, // for the seek
                FileOperation::ReadAt { offset, count: 1 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_set_attrs() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_WRITABLE);
        let mut set_attrs = NodeAttributes {
            mode: 0,
            id: 0,
            content_size: 0,
            storage_size: 0,
            link_count: 0,
            creation_time: 40000,
            modification_time: 100000,
        };
        let status = env
            .proxy
            .set_attr(
                NODE_ATTRIBUTE_FLAG_CREATION_TIME | NODE_ATTRIBUTE_FLAG_MODIFICATION_TIME,
                &mut set_attrs,
            )
            .await
            .unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(
            *events,
            vec![
                FileOperation::Init { flags: OPEN_RIGHT_WRITABLE },
                FileOperation::SetAttrs {
                    flags: NODE_ATTRIBUTE_FLAG_CREATION_TIME
                        | NODE_ATTRIBUTE_FLAG_MODIFICATION_TIME,
                    attrs: set_attrs
                },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_set_flags() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_WRITABLE);
        let status = env.proxy.set_flags(OPEN_FLAG_APPEND).await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        let (status, flags) = env.proxy.get_flags().await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        assert_eq!(flags, OPEN_RIGHT_WRITABLE | OPEN_FLAG_APPEND);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_sync() {
        let env = init_mock_file(Box::new(always_succeed_callback), 0);
        let () = env.proxy.sync().await.unwrap().map_err(zx::Status::from_raw).unwrap();
        let events = env.file.operations.lock().unwrap();
        assert_eq!(*events, vec![FileOperation::Init { flags: 0 }, FileOperation::Sync,]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_truncate() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_WRITABLE);
        let status = env.proxy.truncate(10).await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        let events = env.file.operations.lock().unwrap();
        assert_matches!(
            &events[..],
            [
                FileOperation::Init { flags: OPEN_RIGHT_WRITABLE },
                FileOperation::Truncate { length: 10 },
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_truncate_no_perms() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let status = env.proxy.truncate(10).await.unwrap();
        assert_eq!(zx::Status::from_raw(status), zx::Status::BAD_HANDLE);
        let events = env.file.operations.lock().unwrap();
        assert_eq!(*events, vec![FileOperation::Init { flags: OPEN_RIGHT_READABLE },]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_write() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_WRITABLE);
        let data = "Hello, world!".as_bytes();
        let count = env.proxy.write(data).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(count, data.len() as u64);
        let events = env.file.operations.lock().unwrap();
        assert_matches!(
            &events[..],
            [
                FileOperation::Init { flags: OPEN_RIGHT_WRITABLE },
                FileOperation::WriteAt { offset: 0, .. },
            ]
        );
        if let FileOperation::WriteAt { content, .. } = &events[1] {
            assert_eq!(content.as_slice(), data);
        } else {
            unreachable!();
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_write_no_perms() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_READABLE);
        let data = "Hello, world!".as_bytes();
        let result = env.proxy.write(data).await.unwrap().map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::BAD_HANDLE));
        let events = env.file.operations.lock().unwrap();
        assert_eq!(*events, vec![FileOperation::Init { flags: OPEN_RIGHT_READABLE },]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_write_at() {
        let env = init_mock_file(Box::new(always_succeed_callback), OPEN_RIGHT_WRITABLE);
        let data = "Hello, world!".as_bytes();
        let count =
            env.proxy.write_at(data, 10).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(count, data.len() as u64);
        let events = env.file.operations.lock().unwrap();
        assert_matches!(
            &events[..],
            [
                FileOperation::Init { flags: OPEN_RIGHT_WRITABLE },
                FileOperation::WriteAt { offset: 10, .. },
            ]
        );
        if let FileOperation::WriteAt { content, .. } = &events[1] {
            assert_eq!(content.as_slice(), data);
        } else {
            unreachable!();
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_append() {
        let env = init_mock_file(
            Box::new(always_succeed_callback),
            OPEN_RIGHT_WRITABLE | OPEN_FLAG_APPEND,
        );
        let data = "Hello, world!".as_bytes();
        let count = env.proxy.write(data).await.unwrap().map_err(zx::Status::from_raw).unwrap();
        assert_eq!(count, data.len() as u64);
        let offset = env
            .proxy
            .seek(SeekOrigin::Current, 0)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();
        assert_eq!(offset, *MOCK_FILE_SIZE + data.len() as u64);
        let events = env.file.operations.lock().unwrap();
        const INIT_FLAGS: u32 = OPEN_RIGHT_WRITABLE | OPEN_FLAG_APPEND;
        assert_matches!(
            &events[..],
            [FileOperation::Init { flags: INIT_FLAGS }, FileOperation::Append { .. },]
        );
        if let FileOperation::Append { content } = &events[1] {
            assert_eq!(content.as_slice(), data);
        } else {
            unreachable!();
        }
    }
}
