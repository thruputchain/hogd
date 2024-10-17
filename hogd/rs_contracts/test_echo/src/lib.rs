#![no_std]

use rs_contract_sdk as sdk;

use core::mem::MaybeUninit;
use core::ptr::copy_nonoverlapping;
use echo;
use sdk::rt::{trap, unwrap_abort};
use rs_macros::public_method;

extern crate alloc;


#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn test_slice() -> i32 {
    let calldata = unwrap_abort(sdk::GetCalldata());

    if calldata.len() < 32 + 4 + 4 + 4 {
        trap();
    }

    let mut call_addr_mu = MaybeUninit::<sdk::ContractAddress>::uninit();

    unsafe {
        copy_nonoverlapping(
            calldata.as_ptr(),
            call_addr_mu.as_mut_ptr() as *mut u8,
            32,
        );
    }

    let call_addr = unsafe { call_addr_mu.assume_init() };

    let method: sdk::MethodID =
        u32::from_le_bytes(unwrap_abort(calldata[32..36].try_into()));

    let slice_start =
        u32::from_le_bytes(unwrap_abort(calldata[36..40].try_into())) as usize;
    let slice_end =
        u32::from_le_bytes(unwrap_abort(calldata[40..44].try_into())) as usize;

    let mut return_len: sdk::RuntimeMemoryLength = 0;

    unwrap_abort(sdk::syscall_bindings::raw_Invoke(
        &call_addr,
        method,
        u64::MAX,
        &calldata[36..],
        &mut return_len,
    ));

    let expected_return_len = (slice_end - slice_start) as u32;

    if return_len != expected_return_len {
        trap();
    }

    let returned_buf = unwrap_abort(sdk::GetReturnedBuffer(return_len));

    unwrap_abort(sdk::Log(&returned_buf));

    0
}


fn test_slice_fancysdk_inner(
    address: sdk::ContractAddress,
    slice_start: u32,
    slice_end: u32,
    calldata: &[u8],
) -> Result<(), i32> {
    let returned_buf =
        echo::echo_calldata(&address, slice_start, slice_end, calldata)?;

    sdk::Log(&returned_buf)?;
    Ok(())
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
#[no_mangle]
pub extern "C" fn test_slice_fancysdk() -> i32 {
    let calldata = unwrap_abort(sdk::GetCalldata());

    match sdk::parse_trait::parse_from_calldata_4::<
        sdk::ContractAddress,
        u32,
        u32,
        &[u8],
        (),
    >(&calldata, test_slice_fancysdk_inner)
    {
        Ok(()) => 0,
        Err(x) => x,
    }
} 


#[public_method]
fn test_slice_fancysdk_inner_v2(
    address: sdk::ContractAddress,
    slice_start: u32,
    slice_end: u32,
    calldata: &[u8],
) -> Result<(), i32> {
    let returned_buf =
        echo::echo_calldata(&address, slice_start, slice_end, calldata)?;

    sdk::Log(&returned_buf)?;
    Ok(())
} 

