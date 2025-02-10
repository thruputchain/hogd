//! Storage for a coin contract

use sdk::result::{ensure, Result};
use sdk::storage::{item_key, FromKey, Map, NNInt};
use sdk::types::{ContractAddress, StorageKey};
use sdk::GetMsgSender;
use serde::{Deserialize, Serialize};

pub type Allowance = NNInt;
pub type Balance = NNInt;

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct CoinStorage {
    pub owner: ContractAddress,
    // pub name: ByteArray,
    // pub symbol: ByteArray,
    pub total_supply: NNInt,
    pub balances: Map<ContractAddress, Balance>,
    pub allowances: Map<(ContractAddress, ContractAddress), Allowance>,
}

impl CoinStorage {
    pub fn new(
        storage_key: &StorageKey,
        owner: &ContractAddress,
    ) -> Result<Self> {
        let key = storage_key.clone();
        let owner = owner.clone();
        let total_supply = NNInt::from_key(item_key(&key, b"1"));
        let balances = Map::from_key(item_key(&key, b"2"));
        let allowances = Map::from_key(item_key(&key, b"3"));
        let storage = CoinStorage {
            owner,
            total_supply,
            balances,
            allowances,
        };

        Ok(storage)
    }

    pub fn balance_of(&self, addr: &ContractAddress) -> Result<Balance> {
        self.balances.get(addr)
    }

    pub fn allowance_for(
        &self,
        owner: &ContractAddress,
        spender: &ContractAddress,
    ) -> Result<Allowance> {
        let key = (*owner, *spender); // XX: Avoid copying
        self.allowances.get(&key)
    }

    pub fn ensure_sender_is_owner(&self) -> Result<()> {
        let sender = GetMsgSender()?;
        ensure(sender == self.owner, "sender is owner")
    }
}
