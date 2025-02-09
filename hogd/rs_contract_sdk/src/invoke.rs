#![allow(non_snake_case)]

use crate::sys;
use crate::types::{ContractAddress, RuntimeMemoryLength, SHA256Hash};
use crate::result::SysResult as Result;
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
    sys::raw_Exit(
        crate::syscall::SyscallErrorCode::kDeterministicWasm as i32,
    );
}

pub fn GetSourceTransactionHash() -> Result<SHA256Hash> {
    let mut out_hash: MaybeUninit<SHA256Hash> =
        MaybeUninit::<SHA256Hash>::uninit();
    sys::uninit_GetSourceTransactionHash(&mut out_hash)?;
    Ok(unsafe { out_hash.assume_init() })
}

#[cfg(any(feature = "alloc", feature = "std"))]
mod get_calldata_alloc
{
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

#[cfg(any(feature = "alloc", feature = "std"))]
pub use get_calldata_alloc::*;

#[cfg(any(feature = "alloc", feature = "std"))]
mod handle
{
    use serde::Serialize;
    use serde::de::DeserializeOwned;
    use bcs;

    use super::{GetCalldata, Return};
    use crate::syscall::{SysErr, SyscallErrorCode};

    // Run a handler function on the invocation arguments
    // available in serialized form via `GetCalldata`,
    // and serialize the result for passing to `Return`.
    pub fn handle_call<F, ARGS, RES>(f: F) -> SysErr
        where F: FnOnce(ARGS) -> RES,
              ARGS: DeserializeOwned,
              RES: Serialize
    {
        let call_data = match GetCalldata() {
            Ok(x) => x,
            Err(e) => return e, // == SyscallErrorCode::kCalldataParseError
        };
        let args: ARGS = match bcs::from_bytes(&call_data) {
            Ok(args) => args,
            Err(_) => return SyscallErrorCode::kCalldataParseError.into()
        };
        let result: RES = f(args);
        let result_bytes = match bcs::to_bytes(&result) {
            Ok(bytes) => bytes,
            Err(_) => return SyscallErrorCode::kCalldataParseError.into()
        };
        Return(result_bytes.as_slice());
    }
}
#[cfg(any(feature = "alloc", feature = "std"))]
pub use handle::handle_call;

#[cfg(any(feature = "alloc", feature = "std"))]
pub mod cross_call {
    use crate::types::{
        ContractAddress,
        RuntimeMemoryLength,
        InvocationFlagSet,
        InvocationFlags,
    };
    use crate::sys;
    use alloc::vec::Vec;
    use serde::Serialize;
    use serde::de::DeserializeOwned;
    use bcs;

    pub fn invoke<ARGS, RET>(
        address: &ContractAddress,
        method: u32,
        gas_limit: u64,
        args: &ARGS) -> RET
        where ARGS: Serialize, RET: DeserializeOwned
    {
        // Encode the args
        let call_data = bcs::to_bytes(args)
            .expect("Couldn't serialize call data");

        // Invoke the method
        let return_buf =
            invoke_inner(address, method, gas_limit, call_data.as_slice());

        // Deserialize the buffer into a return value ...
        let return_buf = &return_buf.as_slice();
        let return_value = bcs::from_bytes(return_buf)
            .expect("Couldn't deserialize return value");

        return_value
    }

    fn invoke_inner(
        address: &ContractAddress,
        method: u32,
        gas_limit: u64,
        call_data: &[u8],
        ) -> Vec<u8>
    {
        // Perform the cross-contract call

        let flags: InvocationFlagSet = InvocationFlags::kNone as InvocationFlagSet;
        sys::raw_Invoke(
            address,
            method,
            gas_limit,
            call_data,
            flags
        ).expect("Contract invocation failed");

        // Retrieve the return-value buffer

        let mut return_len = 0;
        sys::raw_GetReturnedBufferLen(&mut return_len)
            .expect("Couldn't get returned buffer len");

        let mut return_buf = Vec::with_capacity(return_len as usize);
        let mut written: RuntimeMemoryLength = 0;
        sys::raw_GetReturnedBuffer(return_buf.as_mut_slice(), &mut written)
            .expect("Couldn't get returned buffer");

        if written != return_len {
            panic!("Unexpected return buffer length");
        }

        return_buf
    }
}
#[cfg(any(feature = "alloc", feature = "std"))]
pub use cross_call::invoke;
