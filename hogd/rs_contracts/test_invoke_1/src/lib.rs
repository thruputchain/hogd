#![no_std]

use rs_contract_sdk as sdk;

use sdk::rt::trap;

extern crate alloc;

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn test_calldata_success() -> i32 {
    let sender = sdk::GetMsgSender().unwrap();

    sdk::Log(&sender).unwrap();

    let calldata = sdk::GetCalldata().unwrap();

    sdk::Log(&calldata).unwrap();

    0
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
#[no_mangle]
pub extern "C" fn test_calldata_fail() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    sdk::Log(&calldata).unwrap();

    let sender = sdk::GetMsgSender().unwrap();

    sdk::Log(&sender).unwrap();

    trap();
}
