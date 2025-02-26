#![allow(unused)]

use sdk::macros::contract;
use sdk::result::Result;
use sdk::types::ContractAddress;

pub type Amount = i64;

#[contract(mod_name = "client")]
pub trait ERC20 {
    /// Must be called before any other methods, and no more than once.
    fn initialize(&mut self, owner: &ContractAddress) -> Result<()>;

    /// Transfer from caller to `to` address
    fn transfer(&mut self, to: &ContractAddress, amount: Amount) -> Result<()>;

    /// Transfer from `owner` to `to` address, using allowance
    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        to: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;

    /// Adjust allowance for `spender` by `amount` (positive or negative)
    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;

    /// Get balance for given address
    fn get_balance_of(&self, addr: &ContractAddress) -> Result<Amount>;

    /// Get balance for caller
    fn get_balance(&self) -> Result<Amount>;

    /// Create tokens into `to` address
    fn mint(&mut self, to: &ContractAddress, amount: Amount) -> Result<()>;

    /// Destroy tokens from `to` address
    fn burn(&mut self, to: &ContractAddress, amount: Amount) -> Result<()>;
}
