#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::StorageKey;

pub fn NonnegativeInt64Get(key: &StorageKey) -> Result<i64, i32> {
    let mut result: i64 = 0;
    syscall_bindings::raw_NonnegativeInt64Get(key, &mut result)?;
    Ok(result)
}

pub fn NonnegativeInt64SetAdd(
    key: &StorageKey,
    set_value: i64,
    add_value: i64,
) -> Result<(), i32> {
    syscall_bindings::raw_NonnegativeInt64SetAdd(key, set_value, add_value)
}

pub fn NonnegativeInt64Add(
    key: &StorageKey,
    add_value: i64,
) -> Result<(), i32> {
    syscall_bindings::raw_NonnegativeInt64Add(key, add_value)
}
