#![no_std]

use rs_contract_sdk as sdk;
use sdk::rt::trap;

#[cfg_attr(target_family = "wasm", export_name = "0")]
pub extern "C" fn write_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

    let string = &calldata[32..];

    sdk::BytestringSet(&key, &string).unwrap();

    0
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
pub extern "C" fn log_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

    if calldata.len() != 32 {
        trap();
    }

    let bstring = sdk::BytestringGet(&key).unwrap();

    sdk::Log(&bstring).unwrap();

    0
}

#[cfg_attr(target_family = "wasm", export_name = "2")]
pub extern "C" fn expect_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

    let bstring = sdk::BytestringGet(&key).unwrap();
    let expected_bstring = &calldata[32..];

    if bstring.as_ref() != expected_bstring {
        trap();
    }

    0
}
