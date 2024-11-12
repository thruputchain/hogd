#![no_std]

use rs_contract_sdk as sdk;
use sdk::rt::trap;

use rs_macros::{public_interface, public_method};

public_interface! {}

fn assert_root_tx() {
    match sdk::GetMsgSender() {
        Ok(_) => {
            trap();
        }
        Err(x) => {
            if x == sdk::SyscallErrorCode::kIsRootTransaction as i32 {
                return;
            }
            trap();
        }
    }
}

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

    let mut return_len: sdk::RuntimeMemoryLength = 0;

    sdk::syscall_bindings::raw_Invoke(
        &deployed_addr,
        method,
        u64::MAX,
        calldata,
        &mut return_len,
    )
    .unwrap();
}

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn deploy_() -> i32 {
    assert_root_tx();
    let nonce = sdk::GetSourceTransactionHash().unwrap();

    // TODO(geoff): I need a better get_calldata setup when I know precisely what the types should be
    let calldata = sdk::GetCalldata().unwrap();

    if calldata.len() != 32 {
        trap();
    }

    sdk::DeployContract(&nonce, calldata.as_ref().try_into().unwrap()).unwrap();
    0
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
#[no_mangle]
pub extern "C" fn deploy_and_invoke_() -> i32 {
    assert_root_tx();

    let nonce = sdk::GetSourceTransactionHash().unwrap();

    let calldata = sdk::GetCalldata().unwrap();

    if calldata.len() < 36 {
        trap();
    }

    let contract_hash: sdk::SHA256Hash =
        calldata.as_ref()[0..32].try_into().unwrap();

    let rest_calldata = &calldata.as_ref()[36..];

    let deployed_addr = sdk::DeployContract(&nonce, &contract_hash).unwrap();

    let mut return_len: sdk::RuntimeMemoryLength = 0;
    let method: sdk::MethodID =
        u32::from_le_bytes(calldata.as_ref()[32..36].try_into().unwrap());

    sdk::syscall_bindings::raw_Invoke(
        &deployed_addr,
        method,
        u64::MAX,
        &rest_calldata,
        &mut return_len,
    )
    .unwrap();

    0
}
