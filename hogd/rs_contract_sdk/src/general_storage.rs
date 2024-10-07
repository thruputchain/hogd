use crate::syscall::{syscall, to_offset_mut};

use rs_xdr::{SyscallNo, SyscallErrorCode};
use rs_xdr::StorageKey;

use crate::rt::trap;
/*
pub fn has_key(key: &StorageKey) -> Result<KeyStatus, i32> {
    let mut result: u64 = 0;

    let err: u64 = unsafe {
        syscall(
            SyscallNo::HasKey as u64,
            key.as_ptr() as u64,
            to_offset_mut(&mut result) as u64,
            0,
            0,
        )
    };

    if err != SyscallErrorCode::None as u64{
    	return Err(err);
    }

    return match result {
        0 => Ok(KeyStatus::NExist),
        1 => Ok(KeyStatus::Exist),
        2 => Ok(KeyStatus::AlreadyDeleted),
        _ => {
            trap();
        }
    };
} */

pub fn delete_last(key: &StorageKey) -> Result<(), i32> {
    let err: i32 = syscall(SyscallNo::kDeleteLast, key.as_ptr() as u64, 0, 0, 0);

    if err == SyscallErrorCode::kNone as i32
    {
        return Ok(());
    }

    return Err(err);
}


