#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::{StorageKey, StorageObjectType};

pub fn HasKey(key: &StorageKey) -> Result<StorageObjectType, i32> {
    let mut result: i32 = 0;

    syscall_bindings::uninit_HasKey(key, &mut result)?;

    match result {
        x if x == StorageObjectType::kBytestring as i32 => {
            return Ok(StorageObjectType::kBytestring);
        }
        x if x == StorageObjectType::kNonnegativeInt64 as i32 => {
            return Ok(StorageObjectType::kNonnegativeInt64)
        }
        _ => return Err(rs_xdr::SyscallErrorCode::kUnknownEnumVariant as i32),
    }
}

pub fn DeleteLast(key: &StorageKey) -> Result<(), i32> {
    syscall_bindings::raw_DeleteLast(key)
}
