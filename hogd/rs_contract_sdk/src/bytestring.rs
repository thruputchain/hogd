#![allow(non_snake_case)]

use crate::result::SysResult as Result;
use crate::sys;
#[allow(unused_imports)]
use crate::syscall::SyscallErrorCode;
use crate::types::{RuntimeMemoryLength, StorageKey};
use core::mem::MaybeUninit;

#[cfg(feature = "alloc")]
pub use bytestring_get_alloc::BytestringGet;

#[cfg(feature = "alloc")]
#[doc(alias = "BytestringExists")]
/// Returns `true` when a bytestring exists at the given key, and
/// `false` if nothing does.  If an item of another type exists there,
/// returns an error with code [`SyscallErrorCode::kInvalidInput`].
pub fn bytestring_exists(key: &StorageKey) -> Result<bool> {
    match BytestringGetLen(key) {
        Ok(_) => Ok(true),
        Err(error) => {
            if error.is_storage_key_not_exist() {
                Ok(false)
            } else {
                Err(error)
            }
        }
    }
}

pub fn BytestringGetLen(key: &StorageKey) -> Result<RuntimeMemoryLength> {
    let mut out: RuntimeMemoryLength = 0;
    sys::raw_BytestringGetLen(key, &mut out)?;

    Ok(out)
}

pub fn BytestringSet(key: &StorageKey, slice: &[u8]) -> Result<()> {
    sys::raw_BytestringSet(key, slice, 0)?;
    Ok(())
}

pub fn BytestringSetExclusive(key: &StorageKey, slice: &[u8]) -> Result<()> {
    sys::raw_BytestringSet(
        key,
        slice,
        crate::types::StorageDeltaModifier::kLockExclusive as i32,
    )
}

pub fn BytestringGet_Sized<const N: usize>(
    key: &StorageKey,
) -> Result<[u8; N]> {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); N];
    let mut len: RuntimeMemoryLength = 0;
    sys::uninit_BytestringGet(key, &mut bytes_mu, &mut len)?;
    if len != N as u32 {
        panic!("Unexpected bytestring size");
    }

    let bytes: [u8; N] = unsafe { MaybeUninit::array_assume_init(bytes_mu) };
    Ok(bytes)
}

#[cfg(any(feature = "std", feature = "alloc"))]
mod bytestring_get_alloc {

    extern crate alloc;
    use alloc::boxed::Box;

    use crate::result::SysResult as Result;
    use crate::sys;
    use crate::types::{RuntimeMemoryLength, StorageKey};

    pub fn BytestringGet(key: &StorageKey) -> Result<Box<[u8]>> {
        let len: RuntimeMemoryLength = super::BytestringGetLen(key)?;
        let mut vec = Box::<[u8]>::new_uninit_slice(len as usize);
        let mut written_len: RuntimeMemoryLength = 0;
        sys::uninit_BytestringGet(key, &mut vec, &mut written_len)?;
        if written_len != len {
            panic!("Inconsistent bytestring size");
        }

        Ok(unsafe { vec.assume_init() })
    }
}
