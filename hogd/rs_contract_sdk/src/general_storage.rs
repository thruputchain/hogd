use crate::syscall::{SyscallNo, SyscallErrorCode, syscall, to_offset_mut};

use crate::types::StorageKey;

use crate::rt::trap;

// defined in syscalls.md
#[repr(u64)]
pub enum KeyStatus {
    NExist = 0,
    Exist = 1,
    AlreadyDeleted = 2,
}

pub fn has_key(key: &StorageKey) -> Result<KeyStatus, u64> {
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
}

pub fn delete_last(key: &StorageKey) -> Result<(), u64> {
    let err: u64 = unsafe { syscall(SyscallNo::DeleteLast as u64, key.as_ptr() as u64, 0, 0, 0) };

    if err == SyscallErrorCode::None as u64
    {
        return Ok(());
    }

    return Err(err);
}


