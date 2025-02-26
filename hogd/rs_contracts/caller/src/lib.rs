//! Example caller of ERC20 contract

#![no_std]

#[cfg(feature = "std")]
extern crate std;

#[cfg(all(feature = "alloc", not(feature = "std")))]
extern crate alloc;

use erc20::interface::{Amount, ERC20};
use erc20::interface::client::ERC20Client;
use sdk::macros::contract;
use sdk::result::Result;
use sdk::types::ContractAddress;

pub trait CallerInterface {
    fn check_balance(&self, account: &ContractAddress) -> Result<Amount>;
}

pub struct Caller {
    token_contract: ContractAddress,
}

impl Caller {
    pub fn new(token_contract: &ContractAddress) -> Self {
        let token_contract = token_contract.clone();
        Self { token_contract }
    }
}

#[contract]
impl CallerInterface for Caller {
    fn check_balance(&self, account: &ContractAddress) -> Result<Amount> {
        let token = ERC20Client::new(&self.token_contract);

        token.get_balance_of(account)
    }
}
