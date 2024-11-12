#![no_std]

use rs_contract_sdk as sdk;

use rs_macros::public_method;

extern crate alloc;
use alloc::boxed::Box;

rs_macros::public_interface!();

// Returns err if something goes wrong, otherwise traps
// (weird behavior, but useful for this test case)
#[public_method]
fn deploy_invoke_then_revert(
    nonce: &sdk::SHA256Hash,
    contract_hash: &sdk::SHA256Hash,
) -> Result<(), i32> {
    let echo_addr = sdk::DeployContract(nonce, contract_hash)?;

    let echo = echo::echo::new(&echo_addr);

    let echo_data: [u8; 4] = [0, 1, 2, 3];

    let returned_buf: Box<[u8]> = echo.echo_calldata_fancy(0, 4, &echo_data)?;

    if returned_buf.as_ref() != echo_data {
        return Err(100);
    }

    sdk::rt::trap();
}
