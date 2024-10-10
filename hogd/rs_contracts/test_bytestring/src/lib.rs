#![no_std]

use rs_contract_sdk as sdk;

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn write_bytestring() -> i32 {

    let calldata = sdk::GetCalldata().unwrap();

    let key : sdk::StorageKey = calldata[0..32].try_into().unwrap();

    let string = &calldata[32..];

    sdk::BytestringSet(&key, &string).unwrap();

    0
}
