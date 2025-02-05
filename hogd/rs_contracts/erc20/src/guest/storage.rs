//! Storage for a coin contract

use sdk::types::{ContractAddress, StorageKey};
use sdk::result::{Result, assert};
use sdk::storage::{NNInt, Map, FromKey, item_key};
use serde::{Serialize, Deserialize};

/// SDK item
pub fn sender() -> Result<ContractAddress> {
    sdk::GetMsgSender().map_err(From::from)
}

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
    pub fn new(storage_key: &StorageKey, owner: &ContractAddress) -> Result<Self> {
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

    pub fn allowance_for(&self, owner: &ContractAddress, spender: &ContractAddress)
        -> Result<Allowance>
    {
        let key = (*owner, *spender);   // XX: Avoid copying
        self.allowances.get(&key)
    }

    pub fn assert_sender_is_owner(&self) -> Result<()> {
        let sender = sender()?;
        assert(sender == self.owner, "sender is owner")
    }
}
