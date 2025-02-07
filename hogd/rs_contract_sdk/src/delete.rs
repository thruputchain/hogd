#![allow(non_snake_case)]

use crate::syscall_bindings;
use groundhog_xdr::StorageKey;

pub fn DeleteObject(key: &StorageKey) -> Result<(), i32> {
    syscall_bindings::raw_DeleteObject(key)
}
