#![allow(non_snake_case)]

use crate::syscall_bindings;

use core::mem::MaybeUninit;
use rs_xdr::{ContractAddress, RuntimeMemoryLength};

pub fn GetMsgSender() -> Result<ContractAddress, i32> {
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();
    syscall_bindings::uninit_GetMsgSender(&mut out_addr)?;
    Ok(unsafe { out_addr.assume_init() })
}

pub fn GetCalldataLen() -> Result<RuntimeMemoryLength, i32> {
    let mut out: RuntimeMemoryLength = 0;
    syscall_bindings::raw_GetCalldataLen(&mut out)?;
    Ok(out)
}

pub fn Return(return_buf: &[u8]) -> ! {
    syscall_bindings::raw_Return(return_buf);
}

#[cfg(feature = "alloc")]
mod get_calldata_alloc {

    use rs_xdr::RuntimeMemoryLength;

    extern crate alloc;
    use alloc::boxed::Box;
    use alloc::vec::Vec;

    use crate::rt::trap;
    use crate::syscall_bindings;

    pub fn GetCalldata() -> Result<Box<[u8]>, i32> {
        let len: RuntimeMemoryLength = crate::invoke::GetCalldataLen()?;

        let mut vec_mu = Vec::with_capacity(len as usize);

        unsafe {
            vec_mu.set_len(len as usize);
        }

        let mut written_len: RuntimeMemoryLength = 0;

        let mut vec = vec_mu.into_boxed_slice();

        syscall_bindings::raw_GetCalldata(&mut vec, &mut written_len)?;

        if written_len != len {
            trap();
        }

        Ok(vec)
    }

    pub fn GetReturnedBuffer(
        expected_len: RuntimeMemoryLength,
    ) -> Result<Box<[u8]>, i32> {
        let mut vec_mu = Vec::with_capacity(expected_len as usize);

        unsafe {
            vec_mu.set_len(expected_len as usize);
        }

        let mut vec = vec_mu.into_boxed_slice();

        let mut written_len: RuntimeMemoryLength = 0;

        syscall_bindings::raw_GetReturnedBuffer(&mut vec, &mut written_len)?;

        if written_len != expected_len {
            trap();
        }

        Ok(vec)
    }
}

#[cfg(feature = "alloc")]
pub use get_calldata_alloc::*;
