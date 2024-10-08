#![allow(non_snake_case)]

use crate::syscall_bindings;
use core::mem::MaybeUninit;
use rs_xdr::{StorageKey, StorageObjectType};

pub fn HasKey(key: &StorageKey) -> Result<StorageObjectType, i32> {
    let mut result: MaybeUninit<StorageObjectType> =
        MaybeUninit::<StorageObjectType>::uninit();

    syscall_bindings::uninit_HasKey(key, &mut result)?;

    Ok(unsafe { result.assume_init() })
}

pub fn DeleteLast(key: &StorageKey) -> Result<(), i32> {
    syscall_bindings::raw_DeleteLast(key)
}
