//! Storage for a coin contract

use sdk::result::Result;
use sdk::storage::{Keypath, FromKeypath, Map, NNInt};
use sdk::types::ContractAddress;
use serde::{Deserialize, Serialize};

pub type Allowance = NNInt;
pub type Balance = NNInt;

#[derive(Debug, Serialize, Deserialize)]
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
        keypath: &Keypath,
        owner: &ContractAddress,
    ) -> Result<Self> {
        let owner = owner.clone();

        // XXX
        let total_supply = NNInt::from_keypath(keypath.cloned_push_item(b"1"))?;
        let balances = Map::from_keypath(keypath.cloned_push_item(b"2"))?;
        let allowances = Map::from_keypath(keypath.cloned_push_item(b"3"))?;

        let storage = CoinStorage {
            owner,
            total_supply,
            balances,
            allowances,
        };
        Ok(storage)
    }

    pub fn contract_owner(&self) -> &ContractAddress {
        &self.owner
    }

    pub fn balance_of(&self, addr: &ContractAddress) -> Result<Balance> {
        self.balances.get(addr)
    }

    pub fn allowance_for(
        &self,
        owner: &ContractAddress,
        spender: &ContractAddress,
    ) -> Result<Allowance> {
        let key = (*owner, *spender); // XX: Avoid copying?
        self.allowances.get(&key)
    }
}
