// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Common utilities used by several directory implementations.

use crate::{common::stricter_or_same_rights, directory::entry::EntryInfo};

use {
    byteorder::{LittleEndian, WriteBytesExt},
    fidl_fuchsia_io::{
        CLONE_FLAG_SAME_RIGHTS, MAX_FILENAME, MODE_TYPE_DIRECTORY, MODE_TYPE_MASK,
        OPEN_FLAGS_ALLOWED_WITH_NODE_REFERENCE, OPEN_FLAG_APPEND, OPEN_FLAG_CREATE,
        OPEN_FLAG_CREATE_IF_ABSENT, OPEN_FLAG_DESCRIBE, OPEN_FLAG_DIRECTORY,
        OPEN_FLAG_NODE_REFERENCE, OPEN_FLAG_NOT_DIRECTORY, OPEN_FLAG_POSIX_DEPRECATED,
        OPEN_FLAG_POSIX_EXECUTABLE, OPEN_FLAG_POSIX_WRITABLE, OPEN_FLAG_TRUNCATE,
        OPEN_RIGHT_EXECUTABLE, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE,
    },
    fuchsia_zircon as zx,
    static_assertions::assert_eq_size,
    std::{io::Write, mem::size_of},
};

/// Checks flags provided for a new connection.  Returns adjusted flags (cleaning up some
/// ambiguities) or a fidl Status error, in case new new connection flags are not permitting the
/// connection to be opened.
///
/// OPEN_FLAG_NODE_REFERENCE is preserved.
///
/// Changing this function can be dangerous!  Flags operations may have security implications.
pub fn new_connection_validate_flags(mut flags: u32) -> Result<u32, zx::Status> {
    if flags & OPEN_FLAG_NODE_REFERENCE != 0 {
        flags &= OPEN_FLAGS_ALLOWED_WITH_NODE_REFERENCE;
    }

    if flags & OPEN_FLAG_DIRECTORY != 0 {
        flags &= !OPEN_FLAG_DIRECTORY;
    }

    if flags & OPEN_FLAG_NOT_DIRECTORY != 0 {
        return Err(zx::Status::NOT_FILE);
    }

    // Explicitly expand OPEN_FLAG_POSIX_DEPRECATED to prevent right escalation issues.
    // TODO(fxbug.dev/81185): Remove this branch and usesof OPEN_FLAG_POSIX_DEPRECATED below when
    // out-of-tree clients have been updated.
    if (flags & OPEN_FLAG_POSIX_DEPRECATED) != 0 {
        flags |= OPEN_FLAG_POSIX_WRITABLE | OPEN_FLAG_POSIX_EXECUTABLE;
    }
    // Parent connection must check the POSIX flags in `check_child_connection_flags`, so if any
    // are still present, we expand their respective rights and remove any remaining flags.
    if flags & (OPEN_FLAG_POSIX_DEPRECATED | OPEN_FLAG_POSIX_EXECUTABLE) != 0 {
        flags |= OPEN_RIGHT_EXECUTABLE;
    }
    if flags & (OPEN_FLAG_POSIX_DEPRECATED | OPEN_FLAG_POSIX_WRITABLE) != 0 {
        flags |= OPEN_RIGHT_WRITABLE;
    }
    flags &= !(OPEN_FLAG_POSIX_DEPRECATED | OPEN_FLAG_POSIX_WRITABLE | OPEN_FLAG_POSIX_EXECUTABLE);

    let allowed_flags = OPEN_FLAG_NODE_REFERENCE
        | OPEN_FLAG_DESCRIBE
        | OPEN_FLAG_CREATE
        | OPEN_FLAG_CREATE_IF_ABSENT
        | OPEN_FLAG_DIRECTORY
        | OPEN_RIGHT_READABLE
        | OPEN_RIGHT_WRITABLE
        | OPEN_RIGHT_EXECUTABLE;

    let prohibited_flags = OPEN_FLAG_APPEND | OPEN_FLAG_TRUNCATE;

    if flags & prohibited_flags != 0 {
        return Err(zx::Status::INVALID_ARGS);
    }

    if flags & !allowed_flags != 0 {
        return Err(zx::Status::NOT_SUPPORTED);
    }

    Ok(flags)
}

/// Directories need to make sure that connections to child entries do not receive more rights than
/// the connection to the directory itself.  Plus there is special handling of the OPEN_FLAG_POSIX_*
/// flags. This function should be called before calling [`new_connection_validate_flags`] if both
/// are needed.
pub fn check_child_connection_flags(
    parent_flags: u32,
    mut flags: u32,
    mut mode: u32,
) -> Result<(u32, u32), zx::Status> {
    let mode_type = mode & MODE_TYPE_MASK;

    if mode_type == 0 {
        if flags & OPEN_FLAG_DIRECTORY != 0 {
            mode |= MODE_TYPE_DIRECTORY;
        }
    } else {
        if flags & OPEN_FLAG_DIRECTORY != 0 && mode_type != MODE_TYPE_DIRECTORY {
            return Err(zx::Status::INVALID_ARGS);
        }

        if flags & OPEN_FLAG_NOT_DIRECTORY != 0 && mode_type == MODE_TYPE_DIRECTORY {
            return Err(zx::Status::INVALID_ARGS);
        }
    }

    if flags & (OPEN_FLAG_NOT_DIRECTORY | OPEN_FLAG_DIRECTORY)
        == OPEN_FLAG_NOT_DIRECTORY | OPEN_FLAG_DIRECTORY
    {
        return Err(zx::Status::INVALID_ARGS);
    }

    // Can only specify OPEN_FLAG_CREATE_IF_ABSENT if OPEN_FLAG_CREATE is also specified.
    if flags & OPEN_FLAG_CREATE_IF_ABSENT != 0 && flags & OPEN_FLAG_CREATE == 0 {
        return Err(zx::Status::INVALID_ARGS);
    }

    // Can only use CLONE_FLAG_SAME_RIGHTS when calling Clone.
    if flags & CLONE_FLAG_SAME_RIGHTS != 0 {
        return Err(zx::Status::INVALID_ARGS);
    }

    // Expand POSIX flag into new equivalents.
    // TODO(fxbug.dev/81185): Remove branch once all out-of-tree clients have been updated.
    if flags & OPEN_FLAG_POSIX_DEPRECATED != 0 {
        flags |= OPEN_FLAG_POSIX_WRITABLE | OPEN_FLAG_POSIX_EXECUTABLE;
        flags &= !OPEN_FLAG_POSIX_DEPRECATED;
    }
    // Remove POSIX flags when the respective rights are not available ("soft fail").
    if parent_flags & OPEN_RIGHT_EXECUTABLE == 0 {
        flags &= !OPEN_FLAG_POSIX_EXECUTABLE;
    }
    if parent_flags & OPEN_RIGHT_WRITABLE == 0 {
        flags &= !OPEN_FLAG_POSIX_WRITABLE;
    }

    // Can only use CREATE flags if the parent connection is writable.
    if flags & OPEN_FLAG_CREATE != 0 && parent_flags & OPEN_RIGHT_WRITABLE == 0 {
        return Err(zx::Status::ACCESS_DENIED);
    }

    if stricter_or_same_rights(parent_flags, flags) {
        Ok((flags, mode))
    } else {
        Err(zx::Status::ACCESS_DENIED)
    }
}

#[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95027)
/// A helper to generate binary encodings for the ReadDirents response.  This function will append
/// an entry description as specified by `entry` and `name` to the `buf`, and would return `true`.
/// In case this would cause the buffer size to exceed `max_bytes`, the buffer is then left
/// untouched and a `false` value is returned.
pub fn encode_dirent(buf: &mut Vec<u8>, max_bytes: u64, entry: &EntryInfo, name: &str) -> bool {
    let header_size = size_of::<u64>() + size_of::<u8>() + size_of::<u8>();

    assert_eq_size!(u64, usize);

    if buf.len() + header_size + name.len() > max_bytes as usize {
        return false;
    }

    assert!(
        name.len() <= MAX_FILENAME as usize,
        "Entry names are expected to be no longer than MAX_FILENAME ({}) bytes.\n\
         Got entry: '{}'\n\
         Length: {} bytes",
        MAX_FILENAME,
        name,
        name.len()
    );

    assert!(
        MAX_FILENAME <= u8::max_value() as u64,
        "Expecting to be able to store MAX_FILENAME ({}) in one byte.",
        MAX_FILENAME
    );

    buf.write_u64::<LittleEndian>(entry.inode())
        .expect("out should be an in memory buffer that grows as needed");
    buf.write_u8(name.len() as u8).expect("out should be an in memory buffer that grows as needed");
    buf.write_u8(entry.type_()).expect("out should be an in memory buffer that grows as needed");
    buf.write(name.as_ref()).expect("out should be an in memory buffer that grows as needed");

    true
}

#[cfg(test)]
mod tests {
    use super::{check_child_connection_flags, new_connection_validate_flags};
    use crate::test_utils::build_flag_combinations;

    use {
        fidl_fuchsia_io::{
            CLONE_FLAG_SAME_RIGHTS, MODE_TYPE_DIRECTORY, MODE_TYPE_FILE, OPEN_FLAG_APPEND,
            OPEN_FLAG_CREATE, OPEN_FLAG_CREATE_IF_ABSENT, OPEN_FLAG_DESCRIBE, OPEN_FLAG_DIRECTORY,
            OPEN_FLAG_NODE_REFERENCE, OPEN_FLAG_NOT_DIRECTORY, OPEN_FLAG_POSIX_DEPRECATED,
            OPEN_FLAG_POSIX_EXECUTABLE, OPEN_FLAG_POSIX_WRITABLE, OPEN_FLAG_TRUNCATE,
            OPEN_RIGHT_EXECUTABLE, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE,
        },
        fuchsia_zircon as zx,
    };

    #[track_caller]
    fn ncvf_ok(flags: u32, expected_new_flags: u32) {
        let res = new_connection_validate_flags(flags);
        match res {
            Ok(new_flags) => assert_eq!(
                expected_new_flags, new_flags,
                "new_connection_validate_flags returned unexpected set of flags.\n\
                    Expected: {:X}\n\
                    Actual: {:X}",
                expected_new_flags, new_flags
            ),
            Err(status) => panic!("new_connection_validate_flags failed.  Status: {}", status),
        }
    }

    #[track_caller]
    fn ncvf_err(flags: u32, expected_status: zx::Status) {
        let res = new_connection_validate_flags(flags);
        match res {
            Ok(new_flags) => panic!(
                "new_connection_validate_flags should have failed.  \
                    Got new flags: {:X}",
                new_flags
            ),
            Err(status) => assert_eq!(expected_status, status),
        }
    }

    #[test]
    fn new_connection_validate_flags_node_reference() {
        // OPEN_FLAG_NODE_REFERENCE and OPEN_FLAG_DESCRIBE should be preserved.
        const PRESERVED_FLAGS: u32 = OPEN_FLAG_NODE_REFERENCE | OPEN_FLAG_DESCRIBE;
        for open_flags in build_flag_combinations(
            OPEN_FLAG_NODE_REFERENCE,
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_DESCRIBE | OPEN_FLAG_DIRECTORY,
        ) {
            ncvf_ok(open_flags, open_flags & PRESERVED_FLAGS);
        }

        ncvf_err(OPEN_FLAG_NODE_REFERENCE | OPEN_FLAG_NOT_DIRECTORY, zx::Status::NOT_FILE);
    }

    #[test]
    fn new_connection_validate_flags_posix() {
        // TODO(fxbug.dev/81185): Remove OPEN_FLAG_POSIX_DEPRECATED.
        for open_flags in build_flag_combinations(
            0,
            OPEN_RIGHT_READABLE
                | OPEN_FLAG_POSIX_DEPRECATED
                | OPEN_FLAG_POSIX_EXECUTABLE
                | OPEN_FLAG_POSIX_WRITABLE,
        ) {
            let mut expected_rights = open_flags & OPEN_RIGHT_READABLE;
            if (open_flags & OPEN_FLAG_POSIX_DEPRECATED) != 0 {
                expected_rights |= OPEN_RIGHT_WRITABLE | OPEN_RIGHT_EXECUTABLE;
            }
            if (open_flags & OPEN_FLAG_POSIX_WRITABLE) != 0 {
                expected_rights |= OPEN_RIGHT_WRITABLE
            }
            if (open_flags & OPEN_FLAG_POSIX_EXECUTABLE) != 0 {
                expected_rights |= OPEN_RIGHT_EXECUTABLE
            }
            ncvf_ok(open_flags, expected_rights);
        }
    }

    #[test]
    fn new_connection_validate_flags_create() {
        for open_flags in build_flag_combinations(
            OPEN_FLAG_CREATE,
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE | OPEN_FLAG_CREATE_IF_ABSENT,
        ) {
            ncvf_ok(open_flags, open_flags);
        }
    }

    #[test]
    fn new_connection_validate_flags_append() {
        ncvf_err(OPEN_RIGHT_WRITABLE | OPEN_FLAG_APPEND, zx::Status::INVALID_ARGS);
    }

    #[test]
    fn new_connection_validate_flags_truncate() {
        ncvf_err(OPEN_RIGHT_WRITABLE | OPEN_FLAG_TRUNCATE, zx::Status::INVALID_ARGS);
    }

    #[test]
    fn check_child_connection_flags_create_flags() {
        assert!(check_child_connection_flags(OPEN_RIGHT_WRITABLE, OPEN_FLAG_CREATE, 0).is_ok());
        assert!(check_child_connection_flags(
            OPEN_RIGHT_WRITABLE,
            OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_IF_ABSENT,
            0
        )
        .is_ok());

        assert_eq!(
            check_child_connection_flags(0, OPEN_FLAG_CREATE, 0),
            Err(zx::Status::ACCESS_DENIED),
        );
        assert_eq!(
            check_child_connection_flags(0, OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_IF_ABSENT, 0),
            Err(zx::Status::ACCESS_DENIED),
        );

        // Need to specify OPEN_FLAG_CREATE if passing OPEN_FLAG_CREATE_IF_ABSENT.
        assert_eq!(
            check_child_connection_flags(OPEN_RIGHT_WRITABLE, OPEN_FLAG_CREATE_IF_ABSENT, 0),
            Err(zx::Status::INVALID_ARGS),
        );
    }

    #[test]
    fn check_child_connection_flags_mode() {
        // If mode is 0 but we specify OPEN_FLAG_DIRECTORY, ensure the resulting mode is correct.
        assert_eq!(
            check_child_connection_flags(0, OPEN_FLAG_DIRECTORY, 0)
                .expect("check_child_connection_flags failed")
                .1,
            MODE_TYPE_DIRECTORY
        );

        // Ensure that ambiguous flags/mode types are handled correctly.
        assert_eq!(
            check_child_connection_flags(0, OPEN_FLAG_NOT_DIRECTORY, MODE_TYPE_DIRECTORY),
            Err(zx::Status::INVALID_ARGS),
        );
        assert_eq!(
            check_child_connection_flags(0, OPEN_FLAG_DIRECTORY, MODE_TYPE_FILE),
            Err(zx::Status::INVALID_ARGS),
        );
    }

    #[test]
    fn check_child_connection_flags_invalid() {
        // Cannot specify both OPEN_FLAG_DIRECTORY and OPEN_FLAG_NOT_DIRECTORY.
        assert_eq!(
            check_child_connection_flags(0, OPEN_FLAG_DIRECTORY | OPEN_FLAG_NOT_DIRECTORY, 0),
            Err(zx::Status::INVALID_ARGS),
        );

        // Cannot specify CLONE_FLAG_SAME_RIGHTS when opening a resource (only permitted via clone).
        assert_eq!(
            check_child_connection_flags(0, CLONE_FLAG_SAME_RIGHTS, 0),
            Err(zx::Status::INVALID_ARGS),
        );
    }
}
