#![no_std]

use rs_contract_sdk as sdk;

use rs_macros::{public_interface, public_method};

public_interface! {}

#[public_method(static_calldata_length = 64, static_return_length = 0)]
pub fn deploy(nonce: &sdk::SHA256Hash, contract_hash: &sdk::SHA256Hash) {
    sdk::DeployContract(nonce, contract_hash).unwrap();
}

#[public_method]
pub fn deploy_and_invoke(
    nonce: &sdk::SHA256Hash,
    contract_hash: &sdk::SHA256Hash,
    method: sdk::MethodID,
    calldata: &[u8],
) {
    let deployed_addr = sdk::DeployContract(nonce, contract_hash).unwrap();

    sdk::syscall_bindings::raw_Invoke(
        &deployed_addr,
        method,
        u64::MAX,
        calldata,
    )
    .unwrap();
}
