#![allow(non_snake_case)]

use crate::sys;
use crate::types::StorageKey;
use crate::syscall::SyscallErrorCode;
use crate::result::SysResult as Result;

pub fn NonnegativeIntGet(key: &StorageKey) -> Result<(i128, i128)> {
    let mut set_result: i128 = 0;
    let mut add_result: i128 = 0;
    sys::raw_NonnegativeIntGet(
        key,
        &mut set_result,
        &mut add_result,
    )?;
    Ok((set_result, add_result))
}

pub fn NonnegativeIntSetAdd(
    key: &StorageKey,
    set_value: i128,
    add_value: i128,
) -> Result<()> {
    sys::raw_NonnegativeIntSetAdd(key, &set_value, &add_value)
}

pub fn NonnegativeIntAdd(key: &StorageKey, add_value: i128) -> Result<()> {
    let (set, prev_add) = match NonnegativeIntGet(key) {
        Ok(x) => x,
        Err(e) => {
            if e.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                (0, 0)
            } else {
                return Err(e);
            }
        }
    };

    NonnegativeIntSetAdd(key, set, prev_add + add_value)
}
