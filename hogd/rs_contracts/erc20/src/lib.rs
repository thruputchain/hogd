#![no_std]

use rs_contract_sdk as sdk;
use rs_macros::{public_interface, public_method};

#[cfg(not(feature = "interface"))]
mod config;

#[cfg(not(feature = "interface"))]
mod implementation;

#[cfg(not(feature = "interface"))]
use implementation::*;

//#[cfg(not(feature="interface"))]
//#[no_mangle]
//pub extern mut __stack_pointer: u32 = 0;

public_interface!();

#[public_method(static_calldata_length = 32, static_return_length = 0)]
pub fn initialize(owner: &sdk::ContractAddress) -> () {
    config::write_config_data(&owner);
}

#[public_method(static_calldata_length = 48, static_return_length = 0)]
pub fn transfer(to: &sdk::ContractAddress, amount: i128) {
    let sender = sdk::GetMsgSender().unwrap();
    do_transfer(&sender, &to, amount);
}

#[public_method(static_calldata_length = 80, static_return_length = 0)]
pub fn transfer_from(
    owner: &sdk::ContractAddress,
    to: &sdk::ContractAddress,
    amount: i128,
) {
    let sender = sdk::GetMsgSender().unwrap();

    do_allowance_delta(owner, &sender, -amount);
    do_transfer(owner, to, amount);
}

#[public_method(static_calldata_length = 48, static_return_length = 0)]
pub fn adjust_allowance(spender: &sdk::ContractAddress, amount: i128) {
    let sender = sdk::GetMsgSender().unwrap();

    do_allowance_delta(&sender, spender, amount);
}

#[public_method(static_calldata_length = 32, static_return_length = 16)]
pub fn get_balance_of(addr: &sdk::ContractAddress) -> i128 {
    let balance_key = compute_balance_key(addr);

    match sdk::NonnegativeIntGet(&balance_key) {
        Ok((set, add)) => set + add,
        Err(_) => 0,
    }
}

#[public_method(static_calldata_length = 0, static_return_length = 16)]
pub fn get_balance() -> i128 {
    let sender = sdk::GetMsgSender().unwrap();

    get_balance_of(&sender)
}

#[public_method(static_calldata_length = 48, static_return_length = 0)]
pub fn mint(to: &sdk::ContractAddress, amount: i128) {
    let sender = sdk::GetMsgSender().unwrap();

    let owner = config::load_config_data();
    if owner != sender {
        sdk::rt::trap();
    }

    do_mint(&to, amount);
}

#[public_method(static_calldata_length = 48, static_return_length = 0)]
pub fn burn(to: &sdk::ContractAddress, amount: i128) {
    let sender = sdk::GetMsgSender().unwrap();

    let owner = config::load_config_data();
    if owner != sender {
        sdk::rt::trap();
    }

    do_burn(&to, amount);
}
