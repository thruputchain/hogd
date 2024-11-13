#![no_std]

use rs_contract_sdk as sdk;
use rs_macros::public_method;

#[public_method]
fn root_test_reentrant_revert() {
    let addr = sdk::GetSelfAddress().unwrap();

    let mut return_len: sdk::RuntimeMemoryLength = 0;

    let result = sdk::syscall_bindings::raw_Invoke(
        &addr,
        0,
        u64::MAX,
        &[],
        &mut return_len,
    );

    // test expects result to error before it reaches this point.
    // Thus, we do not unwrap the result, and instead call sdk::Exit(0).
    // If that Exit(0) occurs, the test has failed.
}

#[cfg(not(feature = "interface"))]
#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn revert() -> i32 {
    sdk::rt::trap();
}
