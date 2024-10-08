#![allow(non_snake_case)]

use rs_xdr::{StorageKey, RuntimeMemoryLength};
use crate::syscall_bindings;

pub fn BytestringGetLen(key: &StorageKey) -> Result<RuntimeMemoryLength, i32> {
    let mut out: RuntimeMemoryLength = 0;

    match syscall_bindings::raw_BytestringGetLen(key, &mut out) {
        Ok(()) => Ok(out),
        Err(x) => Err(x)
    }
}

pub fn BytestringSet(key: &StorageKey, slice: &[u8]) -> Result<(), i32> {
    syscall_bindings::raw_BytestringSet(key, slice)
}

// Gating the part of the module that needs an allocator behind a feature flag

#[cfg(feature = "alloc")]
mod bytestring_get_alloc {

    use rs_xdr::{StorageKey, RuntimeMemoryLength};

    extern crate alloc;
    use alloc::boxed::Box;
    use alloc::vec::Vec;

    use crate::syscall_bindings;
    use crate::rt::trap;

    pub fn BytestringGet(key: &StorageKey) -> Result<Box<[u8]>, i32> {
        let len : RuntimeMemoryLength = crate::bytestring::BytestringGetLen(key)?;

        let mut vec_mu = Vec::with_capacity(len as usize);

        unsafe {
            vec_mu.set_len(len as usize);
        }

        let mut written_len: RuntimeMemoryLength = 0;

        let mut vec = vec_mu.into_boxed_slice();

        syscall_bindings::raw_BytestringGet(key, &mut vec, &mut written_len)?;

        if written_len != len {
            trap();
        }

        Ok(vec)
    }
}

#[cfg(feature = "alloc")]
pub use bytestring_get_alloc::BytestringGet;
