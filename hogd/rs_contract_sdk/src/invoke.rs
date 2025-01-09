#![allow(non_snake_case)]

use crate::syscall_bindings;

use core::mem::MaybeUninit;
use rs_xdr::{ContractAddress, RuntimeMemoryLength, SHA256Hash};

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

pub fn GetCalldata_Sized<const N: usize>() -> Result<[u8; N], i32> {
    let mut calldata_mu = [::core::mem::MaybeUninit::<u8>::uninit(); N];

    let mut calldata_len: RuntimeMemoryLength = 0;

    syscall_bindings::uninit_GetCalldata(&mut calldata_mu, &mut calldata_len)?;

    if calldata_len != N as u32 {
        // TODO: what does error handling look like here?
        crate::rt::trap();
    }

    let calldata: [u8; N] =
        unsafe { MaybeUninit::array_assume_init(calldata_mu) };
    Ok(calldata)
}

pub fn Return(return_buf: &[u8]) -> ! {
    syscall_bindings::raw_Return(return_buf);
}

pub fn Exit(code: i32) -> ! {
    syscall_bindings::raw_Exit(code);
}

pub fn Revert() -> ! {
    syscall_bindings::raw_Exit(
        rs_xdr::SyscallErrorCode::kDeterministicWasm as i32,
    );
}

pub fn GetSourceTransactionHash() -> Result<SHA256Hash, i32> {
    let mut out_hash: MaybeUninit<SHA256Hash> =
        MaybeUninit::<SHA256Hash>::uninit();
    syscall_bindings::uninit_GetSourceTransactionHash(&mut out_hash)?;
    Ok(unsafe { out_hash.assume_init() })
}

#[cfg(feature = "alloc")]
mod get_calldata_alloc {

    use rs_xdr::RuntimeMemoryLength;

    extern crate alloc;
    use alloc::boxed::Box;

    use crate::rt::trap;
    use crate::syscall_bindings;

    pub fn GetCalldata() -> Result<Box<[u8]>, i32> {
        let len: RuntimeMemoryLength = crate::invoke::GetCalldataLen()?;

        let mut vec = Box::<[u8]>::new_uninit_slice(len as usize);

        let mut written_len: RuntimeMemoryLength = 0;

        syscall_bindings::uninit_GetCalldata(&mut vec, &mut written_len)?;

        if written_len != len {
            trap();
        }

        Ok(unsafe { vec.assume_init() })
    }

    pub fn GetReturnedBuffer() -> Result<Box<[u8]>, i32> {
        let mut expected_len: RuntimeMemoryLength = 0;
        syscall_bindings::raw_GetReturnedBufferLen(&mut expected_len)?;

        let mut vec = Box::<[u8]>::new_uninit_slice(expected_len as usize);

        let mut written_len: RuntimeMemoryLength = 0;

        syscall_bindings::uninit_GetReturnedBuffer(&mut vec, &mut written_len)?;

        if written_len != expected_len {
            trap();
        }

        Ok(unsafe { vec.assume_init() })
    }
}

#[cfg(feature = "alloc")]
pub use get_calldata_alloc::*;
