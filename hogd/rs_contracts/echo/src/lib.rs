#![no_std]

use rs_contract_sdk as sdk;

use rs_macros::{public_method, public_interface};

extern crate alloc;

#[allow(unused_imports)]
use alloc::boxed::Box;
use alloc::rc::Rc;

public_interface!();
/*
#[cfg(feature="interface")]
pub struct echo<'a> {
    call_address: &'a ::rs_contract_sdk::ContractAddress,
}

#[cfg(feature="interface")]
impl<'a> echo<'a> {
    pub fn new(address: &'a ::rs_contract_sdk::ContractAddress) -> Self {
        echo {call_address : address}
    }
} 
*/


// Echoes back the specified slice of the input calldata
// call format: slice_start slice_end calldata
#[cfg(not(feature = "interface"))]
#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn echo_calldata() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    if calldata.len() < 8 {
        sdk::rt::trap();
    }

    let slice_start =
        8 + u32::from_le_bytes(calldata[0..4].try_into().unwrap()) as usize;
    let slice_end =
        8 + u32::from_le_bytes(calldata[4..8].try_into().unwrap()) as usize;

    sdk::Log(&calldata[slice_start..slice_end]).unwrap();

    sdk::Return(&calldata[slice_start..slice_end]);

    // assert that this code is actually unreachable
    #[allow(unreachable_code)]
    {
        sdk::rt::trap();
    }
}

#[public_method]
pub fn echo_calldata_fancy(
    slice_start: u32,
    slice_end: u32,
    calldata: &[u8]) -> &[u8]
{
    sdk::rt::unwrap_abort_option(calldata.get((slice_start as usize)..(slice_end as usize)))
}

#[cfg(feature = "interface")]
pub fn echo_calldata(
    call_address: &sdk::ContractAddress,
    slice_start: u32,
    slice_end: u32,
    calldata: &[u8],
) -> Result<Box<[u8]>, i32> {
    let buffer_size: usize = 4 + 4 + calldata.len();

    let mut vec = Box::<[u8]>::new_uninit_slice(buffer_size as usize);

    sdk::parse_trait::serialize_to_calldata_3(
        vec.as_mut(),
        slice_start,
        slice_end,
        calldata,
    )?;

    let calldata = unsafe { vec.assume_init() };

    let mut return_len: sdk::RuntimeMemoryLength = 0;

    let method: sdk::MethodID = 0;

    sdk::syscall_bindings::raw_Invoke(
        &call_address,
        method,
        u64::MAX,
        &calldata,
        &mut return_len,
    )?;

    let returned_buf = sdk::GetReturnedBuffer(return_len)?;

    Ok(sdk::parse_trait::parse_returns_from_calldata_1::<Box<[u8]>>(&returned_buf))
}
