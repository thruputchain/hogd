//! Storage for a coin contract

use sdk::result::{ensure, Result};
use sdk::types::ContractAddress;
use sdk::GetMsgSender;
use serde::{Deserialize, Serialize};

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct ConfigStorage {
    pub owner: ContractAddress,
}

impl ConfigStorage {
    pub fn new(owner: &ContractAddress) -> Result<Self> {
        let storage = Self { owner: *owner };

        Ok(storage)
    }

    pub fn ensure_sender_is_owner(&self) -> Result<()> {
        let sender = GetMsgSender()?;
        ensure(sender == self.owner, "sender is owner")
    }
}
