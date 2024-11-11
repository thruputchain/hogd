#![no_std]

use rs_contract_sdk as sdk;

use echo;
use test_deploy_revert_2;

use rs_macros::{public_method, static_key};

extern crate alloc;
use alloc::boxed::Box;

// invokes test_deploy_revert_2, which should revert.
// Then checks to see whether the contract that was deployed can be invoked.
#[public_method]
fn deploy_revert_test_root(
    echo_contract_hash: &sdk::SHA256Hash,
    test_deploy_revert_2_contract_hash: &sdk::SHA256Hash,
) {
    let nonce = static_key! {"nonce"};

    let test_deploy_revert_2_addr =
        sdk::DeployContract(&nonce, test_deploy_revert_2_contract_hash)
            .unwrap();

    let t = test_deploy_revert_2::test_deploy_revert_2::new(
        &test_deploy_revert_2_addr,
    );

    let t_result = t.deploy_invoke_then_revert(&nonce, echo_contract_hash);

    match t_result {
        Err(err_code) => {
            if err_code != sdk::SyscallErrorCode::kDeterministicWasm as i32 {
                sdk::Revert();
            }
        }
        _ => {
            sdk::Revert();
        }
    }

    let echo_addr = sdk::GetDeployAddressForOther(
        &test_deploy_revert_2_addr,
        &nonce,
        echo_contract_hash,
    )
    .unwrap();

    let echo = echo::echo::new(&echo_addr);

    let echo_data: [u8; 4] = [0, 1, 2, 3];

    match echo.echo_calldata_fancy(0, 4, &echo_data) {
        Ok(_) => {
            sdk::rt::trap();
        }
        Err(x) => {
            if x != sdk::SyscallErrorCode::kContractLookupFailed as i32 {
                sdk::Revert();
            }
        }
    }
}
