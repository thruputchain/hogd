use crate::syscall::{syscall, to_offset_mut};
use rs_xdr::{SyscallErrorCode, SyscallNo};

use crate::rt::trap;
use rs_xdr::StorageKey;

pub fn bytestring_get_len(key: &StorageKey) -> Result<u64, i32> {
    let mut out: u64 = 0;
    let err = syscall(
        SyscallNo::kBytestringGetLen,
        key.as_ptr() as u64,
        to_offset_mut(&mut out) as u64,
        0,
        0,
    );

    if err == SyscallErrorCode::kNone as i32 {
        return Ok(out);
    }
    return Err(err);
}

pub fn bytestring_get_len_defaulted(key: &StorageKey) -> Option<u64> {
    match bytestring_get_len(&key) {
        Ok(len) => Some(len),
        Err(err) => {
            if err == SyscallErrorCode::kStorageKeyNExist as i32 {
                return None;
            }
            trap();
        }
    }
}

// returns the number of written bytes; no more than out_slice.len()
pub fn bytestring_get(
    key: &StorageKey,
    out_slice: &mut [u8],
) -> Result<u64, i32> {
    let mut out_len: u64 = 0;
    let err = syscall(
        SyscallNo::kBytestringGet,
        key.as_ptr() as u64,
        out_slice.as_mut_ptr() as u64,
        out_slice.len() as u64,
        to_offset_mut(&mut out_len) as u64,
    );

    if err == SyscallErrorCode::kNone as i32 {
        return Ok(out_len);
    }

    return Err(err);
}

pub fn bytestring_get_defaulted(
    key: &StorageKey,
    out_slice: &mut [u8],
) -> Option<u64> {
    match bytestring_get(key, out_slice) {
        Ok(len) => Some(len),
        Err(err) => {
            if err == SyscallErrorCode::kStorageKeyNExist as i32 {
                return None;
            }
            trap();
        }
    }
}

pub fn bytestring_set(key: &StorageKey, slice: &[u8]) -> Result<(), i32> {
    let err = syscall(
        SyscallNo::kBytestringSet,
        key.as_ptr() as u64,
        slice.as_ptr() as u64,
        slice.len() as u64,
        0,
    );

    if err != SyscallErrorCode::kNone as i32 {
        return Err(err);
    }
    Ok(())
}

// Gating the part of the module that needs an allocator behind a feature flag

#[cfg(feature = "alloc")]
mod bytestring_get_alloc {

    use rs_xdr::StorageKey;

    use core::mem::MaybeUninit;

    extern crate alloc;
    use alloc::boxed::Box;
    use alloc::vec;
    use alloc::vec::Vec;

    use crate::bytestring::bytestring_get_len_defaulted;

    use crate::syscall::{syscall, to_offset_mut};
    use rs_xdr::{SyscallErrorCode, SyscallNo};

    pub fn bytestring_get_alloc(
        key: &StorageKey,
    ) -> Result<Option<Box<[u8]>>, i32> {
        let len: u64 = match bytestring_get_len_defaulted(&key) {
            None => {
                return Ok(None);
            }
            Some(len) => len,
        };

        let mut out: Vec<MaybeUninit<u8>> =
            vec![MaybeUninit::<u8>::uninit(); len.try_into().unwrap()];

        let mut out_len: u64 = 0;

        let err = syscall(
            SyscallNo::kBytestringGet,
            key.as_ptr() as u64,
            out.as_mut_ptr() as u64,
            len as u64,
            to_offset_mut(&mut out_len) as u64,
        );

        // we have already handled the NExist case
        if err == SyscallErrorCode::kNone as i32 {
            return Ok(Some(
                unsafe { core::mem::transmute::<_, Vec<u8>>(out) }
                    .into_boxed_slice(),
            ));
        }
        Err(err)
    }
}

#[cfg(feature = "alloc")]
pub use bytestring_get_alloc::bytestring_get_alloc;
