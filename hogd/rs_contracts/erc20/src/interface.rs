#![allow(unused)]

use sdk::types::ContractAddress;
use sdk::macros::contract;

#[contract(client)]
pub trait ERC20 {
    fn initialize(&mut self, owner: &ContractAddress);
    fn transfer(&mut self, to: &ContractAddress, amount: i64);
    fn transfer_from(&mut self, owner: &ContractAddress,
        to: &ContractAddress, amount: i64);
    fn adjust_allowance(&mut self, spender: &ContractAddress, amount: i64);
    fn get_balance_of(&self, addr: &ContractAddress) -> i64;
    fn get_balance(&self) -> i64;
    fn mint(&mut self, to: &ContractAddress, amount: i64);
    fn burn(&mut self, to: &ContractAddress, amount: i64);
}
