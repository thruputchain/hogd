#![no_std]

use rs_contract_sdk as sdk;

use rs_macros::{public_method, static_key};

// invokes GetDeployAddress with and without an optional argument, and asserts successful return
#[public_method]
fn optional_argument_test_root() {
    let nonce = static_key! {"nonce"};
    let hash = static_key! {"hash"};
    sdk::GetDeployAddress(&nonce, &hash).unwrap();

    let addr = static_key! {"addr"};

    sdk::GetDeployAddressForOther(&addr, &nonce, &hash).unwrap();
}
