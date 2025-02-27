#![allow(unused)]

use sdk::macros::contract;
use sdk::result::Result;
use sdk::types::ContractAddress;

pub type Amount = i128;

#[contract(mod_name = "client")]
pub trait ERC20 {
    fn initialize(&mut self, owner: &ContractAddress) -> Result<()>;
    fn transfer(&mut self, to: &ContractAddress, amount: Amount) -> Result<()>;
    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        to: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;
    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;
    fn get_balance_of(&self, addr: &ContractAddress) -> Result<Amount>;
    fn get_balance(&self) -> Result<Amount>;
    fn mint(&mut self, to: &ContractAddress, amount: Amount) -> Result<()>;
    fn burn(&mut self, to: &ContractAddress, amount: Amount) -> Result<()>;
    fn total_supply(&self) -> Result<Amount>;
}
