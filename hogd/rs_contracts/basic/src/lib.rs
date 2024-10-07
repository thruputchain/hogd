#![no_std]

use rs_contract_sdk as sdk;

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn test_basic() -> u64 {
	let buf : [u8; 3] = [1,2,3];

	let _x : sdk::SHA256Hash = sdk::crypto::sha256(&buf).unwrap();
    return 0;
}
