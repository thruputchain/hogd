#![no_std]

use rs_contract_sdk as sdk;

extern crate alloc;

use alloc::vec::Vec;

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn test_calldata_noalloc() -> i32 {
    let mut len: sdk::RuntimeMemoryLength = 100;

    sdk::syscall_bindings::raw_GetCalldataLen(&mut len).unwrap();

    let mut vec_mu = Vec::with_capacity(len as usize);
    unsafe {
        vec_mu.set_len(len as usize);
    }

    let mut written_len: sdk::RuntimeMemoryLength = 0;

    let mut calldata = vec_mu.into_boxed_slice();

    // uninit vec assume_init() is still a nightly rust feature, for some reason
    sdk::syscall_bindings::raw_GetCalldata(&mut calldata, &mut written_len)
        .unwrap();

    if written_len != len {
        sdk::rt::trap();
    }

    sdk::syscall_bindings::raw_Log(&calldata).unwrap();

    0
}
