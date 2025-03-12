#![allow(non_snake_case)]

use crate::result::SysResult as Result;
use crate::sys;
use crate::types::{ContractAddress, RuntimeMemoryLength, SHA256Hash};
use core::mem::MaybeUninit;

pub fn GetMsgSender() -> Result<ContractAddress> {
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();
    sys::uninit_GetMsgSender(&mut out_addr)?;
    Ok(unsafe { out_addr.assume_init() })
}

pub fn GetCurrentAddress() -> Result<ContractAddress> {
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();
    sys::uninit_GetCurrentAddress(&mut out_addr)?;
    Ok(unsafe { out_addr.assume_init() })
}

pub fn GetCalldataLen() -> Result<RuntimeMemoryLength> {
    let mut out: RuntimeMemoryLength = 0;
    sys::raw_GetCalldataLen(&mut out)?;
    Ok(out)
}

pub fn GetCalldata_Sized<const N: usize>() -> Result<[u8; N]> {
    let mut calldata_mu = [::core::mem::MaybeUninit::<u8>::uninit(); N];

    let mut calldata_len: RuntimeMemoryLength = 0;

    sys::uninit_GetCalldata(&mut calldata_mu, &mut calldata_len)?;

    if calldata_len != N as u32 {
        panic!("Unexpected calldata size");
    }

    let calldata: [u8; N] =
        unsafe { MaybeUninit::array_assume_init(calldata_mu) };
    Ok(calldata)
}

pub fn Return(buf: &[u8]) -> ! {
    sys::raw_Return(buf);
}

pub fn Exit(code: i32) -> ! {
    sys::raw_Exit(code);
}

pub fn Revert() -> ! {
    sys::raw_Exit(crate::syscall::SyscallErrorCode::kDeterministicWasm as i32);
}

pub fn GetSourceTransactionHash() -> Result<SHA256Hash> {
    let mut out_hash: MaybeUninit<SHA256Hash> =
        MaybeUninit::<SHA256Hash>::uninit();
    sys::uninit_GetSourceTransactionHash(&mut out_hash)?;
    Ok(unsafe { out_hash.assume_init() })
}

#[cfg(any(feature = "alloc"))]
pub use get_calldata_alloc::*;

#[cfg(any(feature = "alloc"))]
mod get_calldata_alloc {
    use crate::result::SysResult as Result;
    use crate::sys;
    use crate::types::RuntimeMemoryLength;
    use alloc::boxed::Box;

    pub fn GetCalldata() -> Result<Box<[u8]>> {
        let len: RuntimeMemoryLength = crate::invoke::GetCalldataLen()?;

        let mut vec = Box::<[u8]>::new_uninit_slice(len as usize);

        let mut written_len: RuntimeMemoryLength = 0;

        sys::uninit_GetCalldata(&mut vec, &mut written_len)?;

        if written_len != len {
            panic!("Unexpected calldata size");
        }

        Ok(unsafe { vec.assume_init() })
    }

    pub fn GetReturnedBuffer() -> Result<Box<[u8]>> {
        let mut expected_len: RuntimeMemoryLength = 0;
        sys::raw_GetReturnedBufferLen(&mut expected_len)?;

        let mut vec = Box::<[u8]>::new_uninit_slice(expected_len as usize);

        let mut written_len: RuntimeMemoryLength = 0;

        sys::uninit_GetReturnedBuffer(&mut vec, &mut written_len)?;

        if written_len != expected_len {
            panic!("Unexpected return buffer size");
        }

        Ok(unsafe { vec.assume_init() })
    }
}
