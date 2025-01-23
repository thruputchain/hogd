#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::{RuntimeMemoryLength, StorageKey};

use core::mem::MaybeUninit;

pub fn BytestringGetLen(key: &StorageKey) -> Result<RuntimeMemoryLength, i32> {
    let mut out: RuntimeMemoryLength = 0;

    syscall_bindings::raw_BytestringGetLen(key, &mut out)?;
    Ok(out)
}

pub fn BytestringSet(key: &StorageKey, slice: &[u8]) -> Result<(), i32> {
    syscall_bindings::raw_BytestringSet(key, slice, 0)
}

pub fn BytestringGet_Sized<const N: usize>(
    key: &StorageKey,
) -> Result<[u8; N], i32> {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); N];

    let mut len: RuntimeMemoryLength = 0;

    crate::syscall_bindings::uninit_BytestringGet(
        &key,
        &mut bytes_mu,
        &mut len,
    )?;

    if len != N as u32 {
        crate::rt::trap();
    }

    let bytes: [u8; N] = unsafe { MaybeUninit::array_assume_init(bytes_mu) };
    Ok(bytes)
}

// Gating the part of the module that needs an allocator behind a feature flag

#[cfg(feature = "alloc")]
mod bytestring_get_alloc {

    use rs_xdr::{RuntimeMemoryLength, StorageKey};

    extern crate alloc;
    use alloc::boxed::Box;

    use crate::rt::trap;
    use crate::syscall_bindings;

    pub fn BytestringGet(key: &StorageKey) -> Result<Box<[u8]>, i32> {
        let len: RuntimeMemoryLength =
            crate::bytestring::BytestringGetLen(key)?;

        let mut vec = Box::<[u8]>::new_uninit_slice(len as usize);

        let mut written_len: RuntimeMemoryLength = 0;

        syscall_bindings::uninit_BytestringGet(
            key,
            &mut vec,
            &mut written_len,
        )?;

        if written_len != len {
            trap();
        }

        Ok(unsafe { vec.assume_init() })
    }
}

#[cfg(feature = "alloc")]
pub use bytestring_get_alloc::BytestringGet;
