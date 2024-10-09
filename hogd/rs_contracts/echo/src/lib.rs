#![no_std]

use rs_contract_sdk as sdk;
use sdk::rt::trap;
extern crate alloc;

// Echoes back the specified slice of the input calldata
// call format: slice_start slice_end calldata
#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn echo_calldata() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    if calldata.len() < 8 {
        trap();
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
        trap();
    }
}
