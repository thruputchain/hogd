#![no_std]

use rs_contract_sdk as sdk;

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn test_basic() -> u64 {
    return 0;
}

// rustup target add wasm32-unknown-unknown
