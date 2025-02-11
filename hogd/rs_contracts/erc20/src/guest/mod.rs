//! An implementation of ERC20

mod storage;
use storage::CoinStorage;

use super::interface::{Amount, ERC20Result, ERC20};
use sdk::contract::{Contract, ContractWithStorage};
use sdk::initialize::initialization_guard;
use sdk::macros::contract;
use sdk::result::{ensure, Result};
use sdk::types::{ContractAddress, StorageKey};
use sdk::GetMsgSender;

pub struct CoinContract(ContractWithStorage<CoinStorage>);

use core::ops::Deref;
impl Deref for CoinContract {
    type Target = ContractWithStorage<CoinStorage>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Contract for CoinContract {
    fn new(key: &StorageKey) -> Self {
        Self(ContractWithStorage::new(key))
    }
}

impl CoinContract {
    // A helper function
    pub fn do_transfer(
        &self,
        from: &ContractAddress,
        to: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        ensure(amount >= 0, "transfer amount is non-negative")?;

        self.storage().balances.get(from)?.add(-amount as i128)?;
        self.storage().balances.get(to)?.add(amount as i128)
    }
}

// This version of the interface (and implementation) returns
// error values.  Below is a version that panics.

#[cfg(feature = "result")]
#[contract(mod_name = "exports")]
impl ERC20Result for CoinContract {
    fn initialize(&mut self, owner: &ContractAddress) -> Result<()> {
        initialization_guard()?;

        let storage = CoinStorage::new(self.storage_key(), owner)?;
        self.set_storage(storage);
        Ok(())
    }

    fn transfer(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        let sender = GetMsgSender()?;
        self.do_transfer(&sender, &to, amount)
    }

    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        to: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let sender = GetMsgSender()?;
        let storage = self.storage();

        // If amount is non-negative, do_transfer() will fail
        storage
            .allowance_for(owner, &sender)?
            .add(-amount as i128)?;
        self.do_transfer(owner, to, amount)
    }

    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let sender = GetMsgSender()?;
        self.storage()
            .allowance_for(&sender, spender)?
            .add(amount as i128)
    }

    fn get_balance_of(&self, addr: &ContractAddress) -> Result<Amount> {
        let bal = self.storage().balance_of(addr)?.get()?;
        Ok(bal as Amount)
    }

    fn get_balance(&self) -> Result<Amount> {
        let sender = GetMsgSender()?;
        let bal = self.storage().balance_of(&sender)?.get()?;
        Ok(bal as Amount)
    }

    fn mint(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        let storage = self.storage();
        storage.ensure_sender_is_owner()?;

        ensure(amount >= 0, "mint amount is nonnegative")?;

        let mut balance = storage.balance_of(to)?;
        balance.add(amount as i128)
    }

    fn burn(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        let storage = self.storage();
        storage.ensure_sender_is_owner()?;

        ensure(
            !(amount > 0 || amount == i64::MIN),
            "burn amount is negative",
        )?;

        storage.balance_of(to)?.add(-amount as i128)
    }
}

// This version panics instead of returning error values

#[cfg(not(feature = "result"))]
#[contract(mod_name = "exports")]
impl ERC20 for CoinContract {
    fn initialize(&mut self, owner: &ContractAddress) {
        initialization_guard().unwrap();
        let storage = CoinStorage::new(self.storage_key(), owner).unwrap();
        self.set_storage(storage);
    }

    fn transfer(&mut self, to: &ContractAddress, amount: Amount) {
        let sender = GetMsgSender().unwrap();
        self.do_transfer(&sender, &to, amount).unwrap();
    }

    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        to: &ContractAddress,
        amount: Amount,
    ) {
        let sender = GetMsgSender().unwrap();
        let storage = self.storage();

        // If amount is non-negative, do_transfer() will fail
        storage
            .allowance_for(owner, &sender)
            .unwrap()
            .add(-amount as i128)
            .unwrap();
        self.do_transfer(owner, to, amount).unwrap();
    }

    fn adjust_allowance(&mut self, spender: &ContractAddress, amount: Amount) {
        let sender = GetMsgSender().unwrap();
        self.storage()
            .allowance_for(&sender, spender)
            .unwrap()
            .add(amount as i128)
            .unwrap();
    }

    fn get_balance_of(&self, addr: &ContractAddress) -> Amount {
        self.storage().balance_of(addr).unwrap().get().unwrap() as Amount
    }

    fn get_balance(&self) -> Amount {
        let sender = GetMsgSender().unwrap();
        self.storage().balance_of(&sender).unwrap().get().unwrap() as Amount
    }

    fn mint(&mut self, to: &ContractAddress, amount: Amount) {
        let storage = self.storage();
        storage.ensure_sender_is_owner().unwrap();

        ensure(amount >= 0, "mint amount is nonnegative").unwrap();

        let mut balance = storage.balance_of(to).unwrap();
        balance.add(amount as i128).unwrap();
    }

    fn burn(&mut self, to: &ContractAddress, amount: Amount) {
        let storage = self.storage();
        storage.ensure_sender_is_owner().unwrap();

        ensure(
            !(amount > 0 || amount == i64::MIN),
            "burn amount is negative",
        )
        .unwrap();

        storage
            .balance_of(to)
            .unwrap()
            .add(-amount as i128)
            .unwrap();
    }
}
