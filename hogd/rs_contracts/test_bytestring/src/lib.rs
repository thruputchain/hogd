#![no_std]

use rs_contract_sdk as sdk;
use sdk::rt::trap;

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn write_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

    let string = &calldata[32..];

    sdk::BytestringSet(&key, &string).unwrap();

    0
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
#[no_mangle]
pub extern "C" fn log_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

    if calldata.len() != 32 {
        trap();
    }

    let _bstring = sdk::BytestringGet(&key).unwrap();

    // log not yet impl
    //sdk::Log(&bstring).unwrap();

    0
}
