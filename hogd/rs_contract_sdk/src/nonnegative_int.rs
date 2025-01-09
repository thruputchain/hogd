#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::StorageKey;

pub fn NonnegativeIntGet(key: &StorageKey) -> Result<(i128, i128), i32> {
    let mut set_result: i128 = 0;
    let mut add_result: i128 = 0;
    syscall_bindings::raw_NonnegativeInt64Get(
        key,
        &mut set_result,
        &mut add_result,
    )?;
    Ok(set_result, add_result)
}

pub fn NonnegativeIntSetAdd(
    key: &StorageKey,
    set_value: i128,
    add_value: i128,
) -> Result<(), i32> {
    syscall_bindings::raw_NonnegativeIntSetAdd(key, set_value, add_value)
}

pub fn NonnegativeIntAdd(key: &StorageKey, add_value: i128) -> Result<(), i32> {
    let (set, prev_add) = NonnegativeIntGet(key)?;

    NonnegativeIntSetAdd(key, set, prev_add + add_value)
}
