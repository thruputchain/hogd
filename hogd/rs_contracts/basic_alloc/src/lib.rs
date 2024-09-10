#![no_std]

use rs_contract_sdk as sdk;

extern crate alloc;
use alloc::format;

// Trivial contract, just to make sure we can compile with the allocator
// feature turned on in rs_contract_sdk

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn test_basic() -> u64 {
    let _x = format!("{}", "foo");
    return 0;
}

