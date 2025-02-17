#![no_std]

use sdk::log::Log;
use sdk::types::StorageKey;

#[cfg_attr(target_family = "wasm", export_name = "0")]
pub extern "C" fn write_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key = StorageKey::new(calldata[0..32].try_into().unwrap());

    let string = &calldata[32..];

    sdk::BytestringSet(&key, &string).unwrap();

    0
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
pub extern "C" fn log_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key = StorageKey::new(calldata[0..32].try_into().unwrap());

    if calldata.len() != 32 {
        panic!("bad calldata len");
    }

    let bstring = sdk::BytestringGet(&key).unwrap();

    Log(&bstring).unwrap();

    0
}

#[cfg_attr(target_family = "wasm", export_name = "2")]
pub extern "C" fn expect_bytestring() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key = StorageKey::new(calldata[0..32].try_into().unwrap());

    let bstring = sdk::BytestringGet(&key).unwrap();
    let expected_bstring = &calldata[32..];

    if bstring.as_ref() != expected_bstring {
        panic!("unexpected bytestring");
    }

    0
}
