#![allow(unused)]

use sdk::types::ContractAddress;
use sdk::macros::contract;

pub type Amount = i64;

#[contract(client)]
pub trait ERC20 {

    fn initialize(&mut self, owner: &ContractAddress);
    fn transfer(&mut self, to: &ContractAddress, amount: Amount);
    fn transfer_from(&mut self,
        owner: &ContractAddress,
        to: &ContractAddress,
        amount: Amount);
    fn adjust_allowance(&mut self,
        spender: &ContractAddress,
        amount: Amount);
    fn get_balance_of(&self, addr: &ContractAddress) -> Amount;
    fn get_balance(&self) -> Amount;
    fn mint(&mut self, to: &ContractAddress, amount: Amount);
    fn burn(&mut self, to: &ContractAddress, amount: Amount);
}
