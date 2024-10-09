#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::StorageKey;

pub fn NonnegativeInt64Get(key: &StorageKey) -> Result<i64, i32> {
    let mut result: i64 = 0;
    syscall_bindings::raw_NonnegativeInt64Get(key, &mut result)?;
    Ok(result)
}
