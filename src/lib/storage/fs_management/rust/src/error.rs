// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {fuchsia_zircon::Status, std::ffi::CString, thiserror::Error};

/// The error type used by the `format` and `fsck` filesystem commands.
#[derive(Clone, Debug, Error)]
pub enum CommandError {
    /// A FIDL error occurred.
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    /// There was a problem launching the filesystem process.
    #[error("failed to launch filesystem process: {0}")]
    LaunchProcess(#[from] LaunchProcessError),
    /// An error occurred waiting for the `ZX_PROCESS_TERMINATED` signal on the filesystem process.
    #[error("failed to wait on ZX_PROCESS_TERMINATED signal: {0}")]
    ProcessTerminatedSignal(#[source] Status),
    /// Failed to get the return code of the terminated filesystem process.
    #[error("failed to get return code of process: {0}")]
    GetProcessReturnCode(#[source] Status),
    /// The filesystem process exited with a non-zero return code.
    #[error("process exited with non-zero return code: {0}")]
    ProcessNonZeroReturnCode(i64),
}

/// The error type representing a failure to launch the filesystem process.
#[derive(Clone, Debug, Error)]
#[error(
    "failed to spawn process. launched with {:?}, status: {}, message: {}",
    .args,
    .status,
    .message
)]
pub struct LaunchProcessError {
    pub(super) args: Vec<CString>,
    pub(super) status: Status,
    pub(super) message: String,
}

/// The error type used by the filesystem serve operation.
#[derive(Clone, Debug, Error)]
pub enum ServeError {
    /// A FIDL error occurred.
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    /// There was a problem launching the filesystem process.
    #[error("failed to launch filesystem process: {0}")]
    LaunchProcess(#[from] LaunchProcessError),
}

/// The error type used by the bind operation of a serving filesystem.
#[derive(Clone, Debug, Error)]
pub enum BindError {
    /// A FIDL error occurred.
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    /// An error occurred retrieving the local namespace.
    #[error("failed to get local namespace: {0}")]
    LocalNamespace(#[source] Status),
    /// An error occurred binding the root directory to a path in the local namespace.
    #[error("failed to bind path to local namespace: {0}")]
    Bind(#[source] Status),
}

/// The error type used by the shutdown operation of a serving filesystem.
#[derive(Clone, Debug, Error)]
pub enum ShutdownError {
    /// A FIDL error occurred.
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    /// A request to gracefully shutdown the filesystem using the DirectoryAdmin protocol failed.
    #[error("failed to shutdown filesystem with DirectoryAdmin: {0}")]
    DirectoryAdminUnmount(#[source] Status),
    /// An error occurred waiting for the `ZX_PROCESS_TERMINATED` signal on the filesystem process.
    #[error("failed to wait on ZX_PROCESS_TERMINATED signal: {0}")]
    ProcessTerminatedSignal(#[source] Status),
    /// Failed to get the return code of the terminated filesystem process.
    #[error("failed to get return code of process: {0}")]
    GetProcessReturnCode(#[source] Status),
}

/// The error type used by the query operation of a serving filesystem.
#[derive(Clone, Debug, Error)]
pub enum QueryError {
    /// A FIDL error occurred.
    #[error(transparent)]
    Fidl(#[from] fidl::Error),
    /// A request for filesystem info using the DirectoryAdmin protocol failed.
    #[error("failed to query filesystem with DirectoryAdmin: {0}")]
    DirectoryAdminQuery(#[source] Status),
    /// The filesystem info returned by the DirectoryAdmin protocol was empty.
    #[error("empty filesystem info result")]
    DirectoryAdminEmptyResult,
}

/// The error type used by the kill operation of a serving filesystem.
#[derive(Clone, Copy, Debug, Error)]
pub enum KillError {
    /// A call to `zx_task_kill` failed. The process could not be terminated.
    #[error("zx_task_kill returned: {0}")]
    TaskKill(#[source] Status),
    /// An error occurred waiting for the `ZX_PROCESS_TERMINATED` signal on the filesystem process.
    #[error("failed to wait on ZX_PROCESS_TERMINATED signal: {0}")]
    ProcessTerminatedSignal(#[source] Status),
}