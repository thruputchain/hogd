#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::{RuntimeMemoryLength, StorageKey};

pub fn BytestringGetLen(key: &StorageKey) -> Result<RuntimeMemoryLength, i32> {
    let mut out: RuntimeMemoryLength = 0;

    syscall_bindings::raw_BytestringGetLen(key, &mut out)?;
    Ok(out)
}

pub fn BytestringSet(key: &StorageKey, slice: &[u8]) -> Result<(), i32> {
    syscall_bindings::raw_BytestringSet(key, slice)
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
