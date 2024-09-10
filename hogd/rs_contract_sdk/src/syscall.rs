use crate::rt::trap;
use crate::types::{Address, StorageKey};
use core::mem::MaybeUninit;

/**
 * SyscallNo is defined in designdocs/syscall.md
 * This should ideally be XDR-generated, but that's a long way off.
 */
enum SyscallNo {
    GetMsgSender = 0x0000,
    SHA256 = 0x0001,

    // storage obj read/write
    HasKey = 0x1000,
    DeleteLast = 0x1001,

    BytestringSet = 0x1100,
    BytestringGet = 0x1101,
    BytestringGetLen = 0x1102,
    NNInt64Set = 0x1200,
    NNInt64Get = 0x1201,
    NNInt64GetBase = 0x1202,

    // invoke
    GetCalldata = 0x2000,
    GetCalldataLen = 0x2001,
    Return = 0x2002,
    Invoke = 0x2003,
    GetFullReturn = 0x2004,
}

#[derive(PartialEq)]
#[allow(dead_code)]
enum SyscallErrorCode {
    None = 0x0000_0000,
    IsRootTransaction = 0x0000_0001,
    StorageObjectTypeMismatch = 0x0000_0002,
    StorageKeyNExist = 0x0000_0003,
    StorageKeyAlreadyDeleted = 0x0000_0004,

    NonnegativeInt64SetValueMismatch = 0x0000_0005,

    DoubleReturn = 0x0000_0006,

    InvalidPtrError = 0x0000_0007,
    // anything negative is unrecoverable,
    // and can never be received by the sdk
}

/*
 * Contracts should be free to explicitly access syscall directly if they need different
 * error handling (although the build system is not at the moment set up for that).
 */

#[link(wasm_import_module = "marmot")]
extern "C" {
    fn syscall(syscall: u64, arg1: u64, arg2: u64, arg3: u64, arg4: u64) -> u64;
}

fn to_offset<T>(obj: &T) -> *const u8 {
    unsafe { core::mem::transmute(&obj) }
}

fn to_offset_mut<T>(obj: &mut T) -> *mut u8 {
    unsafe { core::mem::transmute(&obj) }
}

pub fn get_msg_sender() -> Address {
    #[allow(invalid_value)] // initialized by syscall
    let mut out_addr: Address = unsafe {
        core::mem::transmute::<_, Address>(MaybeUninit::<Address>::uninit().assume_init())
    };

    let err: u64 = unsafe {
        // ignore return on this syscall
        syscall(
            SyscallNo::GetMsgSender as u64,
            out_addr.as_mut_ptr() as u64,
            32,
            0,
            0,
        )
    };

    if err != SyscallErrorCode::None as u64 {
        trap();
    }

    out_addr
}

// defined in syscalls.md
pub enum KeyStatus {
    NExist = 0,
    Exist = 1,
    AlreadyDeleted = 2,
}

pub fn has_key(key: &StorageKey) -> KeyStatus {
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

    if err != SyscallErrorCode::None as u64 {
        trap();
    }

    // TODO(geoff): figure out how to get rust to accept that a value is constrained
    return match result {
        0 => KeyStatus::NExist,
        1 => KeyStatus::Exist,
        2 => KeyStatus::AlreadyDeleted,
        _ => {
            trap();
        }
    };
}

pub fn delete_last(key: &StorageKey) -> () {
    let err: u64 = unsafe { syscall(SyscallNo::DeleteLast as u64, key.as_ptr() as u64, 0, 0, 0) };

    if err == SyscallErrorCode::None as u64
        || err == SyscallErrorCode::StorageKeyAlreadyDeleted as u64
    {
        return;
    }

    trap();
}

pub fn bytestring_get_len(key: &StorageKey) -> Option<u64> {
    let mut out: u64 = 0;
    let err: u64 = unsafe {
        syscall(
            SyscallNo::BytestringGetLen as u64,
            key.as_ptr() as u64,
            to_offset_mut(&mut out) as u64,
            0,
            0,
        )
    };

    if err == SyscallErrorCode::StorageKeyNExist as u64 {
        return None;
    }
    if err != SyscallErrorCode::None as u64 {
        trap();
    }
    return Some(out);
}

pub fn bytetring_get(key: &StorageKey, out_slice: &mut [u8]) -> Option<u64> {
    let mut out_len: u64 = 0;
    let err: u64 = unsafe {
        syscall(
            SyscallNo::BytestringGet as u64,
            key.as_ptr() as u64,
            out_slice.as_mut_ptr() as u64,
            out_slice.len() as u64,
            to_offset_mut(&mut out_len) as u64,
        )
    };

    if err == SyscallErrorCode::StorageKeyNExist as u64 {
        return None;
    }

    if err != SyscallErrorCode::None as u64 {
        trap();
    }
    return Some(out_len);
}

pub fn bytestring_set(key: &StorageKey, slice: &[u8]) {
    let err: u64 = unsafe {
        syscall(
            SyscallNo::BytestringSet as u64,
            key.as_ptr() as u64,
            slice.as_ptr() as u64,
            slice.len() as u64,
            0,
        )
    };

    if err != SyscallErrorCode::None as u64 {
        trap();
    }
}
