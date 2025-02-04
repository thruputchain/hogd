#![allow(non_snake_case)]

use crate::sys;
use crate::types::{StorageKey, RuntimeMemoryLength, StorageKey, StorageDeltaModifier};
use core::mem::MaybeUninit;

#[cfg(feature = "alloc")]
pub use bytestring_get_alloc::BytestringGet;

pub fn BytestringGetLen(key: &StorageKey)
    -> Result<RuntimeMemoryLength, i32>
{
    let mut out: RuntimeMemoryLength = 0;
    sys::raw_BytestringGetLen(key, &mut out)?;

    Ok(out)
}

pub fn BytestringSet(key: &StorageKey, slice: &[u8]) -> Result<(), i32> {
    sys::raw_BytestringSet(key, slice, 0)
}

pub fn BytestringSetExclusive(
    key: &StorageKey,
    slice: &[u8],
) -> Result<(), i32> {
    sys::raw_BytestringSet(
        key,
        slice,
        StorageDeltaModifier::kLockExclusive as i32,
    )
}

pub fn BytestringGet_Sized<const N: usize>(key: &StorageKey)
    -> Result<[u8; N], i32>
{
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); N];
    let mut len: RuntimeMemoryLength = 0;
    sys::uninit_BytestringGet(
        &key,
        &mut bytes_mu,
        &mut len,
    )?;
    if len != N as u32 {
        panic!("Unexpected bytestring size");
    }

    let bytes: [u8; N] = unsafe { MaybeUninit::array_assume_init(bytes_mu) };
    Ok(bytes)
}

#[cfg(feature = "alloc")]
mod bytestring_get_alloc {

    extern crate alloc;
    use alloc::boxed::Box;

    use crate::sys;
    use crate::types::{RuntimeMemoryLength, StorageKey};

    pub fn BytestringGet(key: &StorageKey) -> Result<Box<[u8]>, i32> {
        let len: RuntimeMemoryLength = super::BytestringGetLen(key)?;
        let mut vec = Box::<[u8]>::new_uninit_slice(len as usize);
        let mut written_len: RuntimeMemoryLength = 0;
        sys::uninit_BytestringGet(
            key,
            &mut vec,
            &mut written_len,
        )?;
        if written_len != len {
            panic!("Inconsistent bytestring size");
        }

        Ok(unsafe { vec.assume_init() })
    }
}
