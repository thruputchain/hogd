#![no_std]

use rs_contract_sdk as sdk;

use core::mem::MaybeUninit;
use rs_macros::{public_interface, public_method};

public_interface!();

fn compute_balance_key(addr: &sdk::ContractAddress) -> sdk::SHA256Hash
{
    sdk::SHA256(addr).unwrap()
}

fn compute_allowance_key(owner: &sdk::ContractAddress, spender : &sdk::ContractAddress) -> sdk::SHA256Hash
{
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); 64];
    sdk::parse_trait::serialize_to_calldata_2(&mut bytes_mu, owner, spender)
        .unwrap();

    let bytes :[u8; 64] = unsafe {core::mem::transmute(bytes_mu)};

    sdk::SHA256(&bytes).unwrap()
}

fn do_transfer(from: &sdk::ContractAddress, to : &sdk::ContractAddress, amount: i64)
{
    if amount < 0 {
        sdk::rt::trap();
    }

    let from_key = compute_balance_key(from);
    let to_key = compute_balance_key(to);

    sdk::NonnegativeInt64Add(&from_key, -amount).unwrap();
    sdk::NonnegativeInt64Add(&to_key, amount).unwrap();
}

fn allowance_delta(owner: &sdk::ContractAddress, spender : &sdk::ContractAddress, amount: i64)
{
    let balance_key = compute_allowance_key(owner, spender);
    sdk::NonnegativeInt64Add(&balance_key, amount).unwrap();
}

#[public_method(static_calldata_length = 40, static_return_length = 0)]
pub fn transfer(to: &sdk::ContractAddress, amount: i64) {
    let sender = sdk::GetMsgSender().unwrap();
    do_transfer(&sender, &to, amount);
}

#[public_method(static_calldata_length = 72, static_return_length = 0)]
pub fn transfer_from(owner: &sdk::ContractAddress, to: &sdk::ContractAddress, amount: i64) {
    let sender = sdk::GetMsgSender().unwrap();

    allowance_delta(owner, &sender, -amount);
    do_transfer(owner, &to, amount);
}

#[public_method(static_calldata_length = 32, static_return_length = 8)]
pub fn get_balance_of(addr: &sdk::ContractAddress) -> i64 {

    let balance_key = compute_balance_key(addr);

    match sdk::NonnegativeInt64Get(&balance_key) {
        Ok(res) => res,
        Err(_) => 0
    }
}

#[public_method(static_calldata_length = 32, static_return_length = 8)]
pub fn get_balance() -> i64 {
    let sender = sdk::GetMsgSender().unwrap();
    
    get_balance_of(&sender)
}



