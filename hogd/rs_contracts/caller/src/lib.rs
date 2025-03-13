//! Example caller of ERC20 contract

#![no_std]

#[cfg(feature = "std")]
extern crate std;

#[cfg(all(feature = "alloc", not(feature = "std")))]
extern crate alloc;

use erc20::interface::client::ERC20Client;
use erc20::interface::{Amount, ERC20};
use sdk::prelude::*;

#[derive(Debug, Serialize, Deserialize)]
pub struct Caller {
    token_contract: ContractAddress,
}

impl ContractStorage for Caller {}

impl Caller {
    pub fn new(token_contract: &ContractAddress) -> Self {
        Self { token_contract: *token_contract }
    }
}

#[contract]
impl Caller {
    pub fn check_balance(&self, account: &ContractAddress) -> Result<Amount> {
        let token = ERC20Client::new(&self.token_contract);

        token.get_balance_of(account)
    }
}
