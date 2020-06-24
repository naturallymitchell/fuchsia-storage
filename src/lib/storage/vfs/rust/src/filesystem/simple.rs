// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{Filesystem, FilesystemRename},
    crate::directory::helper::DirectlyMutable,
    fuchsia_zircon::Status,
    std::{any::Any, marker::PhantomData, sync::Arc},
};

pub struct SimpleFilesystem<T: DirectlyMutable + 'static> {
    directory_type: PhantomData<T>,
}

impl<T: DirectlyMutable + 'static> SimpleFilesystem<T> {
    pub fn new() -> Self {
        SimpleFilesystem { directory_type: PhantomData }
    }
}

impl<T> FilesystemRename for SimpleFilesystem<T>
where
    T: DirectlyMutable + 'static,
{
    fn rename(
        &self,
        src_dir: Arc<Any + Sync + Send + 'static>,
        src: String,
        dst_dir: Arc<Any + Sync + Send + 'static>,
        dst: String,
    ) -> Result<(), Status> {
        let src_parent = src_dir.downcast::<T>().map_err(|_| Status::INVALID_ARGS)?;
        let dst_parent = dst_dir.downcast::<T>().map_err(|_| Status::INVALID_ARGS)?;

        // We need to lock directories using the same global order, otherwise we risk a deadlock. We
        // will use directory objects memory location to establish global order for the locks.  It
        // introduces additional complexity, but, hopefully, avoids this subtle deadlocking issue.
        //
        // We will lock first object with the smaller memory address.
        let src_order = src_parent.as_ref() as *const dyn DirectlyMutable as *const usize as usize;
        let dst_order = dst_parent.as_ref() as *const dyn DirectlyMutable as *const usize as usize;

        if src_order < dst_order {
            // `unsafe` here indicates that we have checked the global order for the locks for
            // `src_parent` and `dst_parent` and we are calling `rename_from` as `src_parent` has a
            // smaller memory address than the `dst_parent`.
            unsafe {
                src_parent.rename_from(src, Box::new(move |entry| dst_parent.link(dst, entry)))
            }
        } else if src_order == dst_order {
            src_parent.rename_within(src, dst)
        } else {
            // `unsafe` here indicates that we have checked the global order for the locks for
            // `src_parent` and `dst_parent` and we are calling `rename_to` as `dst_parent` has a
            // smaller memory address than the `src_parent`.
            unsafe {
                dst_parent.rename_to(
                    dst,
                    Box::new(move || match src_parent.remove_entry_impl(src)? {
                        None => Err(Status::NOT_FOUND),
                        Some(entry) => Ok(entry),
                    }),
                )
            }
        }
    }
}

impl<T> Filesystem for SimpleFilesystem<T> where T: DirectlyMutable + 'static {}
