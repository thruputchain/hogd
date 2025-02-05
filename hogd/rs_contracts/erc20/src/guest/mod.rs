//! An implementation of ERC20

mod storage;
use storage::CoinStorage;

use storage::sender;
use super::interface::ERC20;
use sdk::initialize::initialization_guard;
use sdk::result::{Result, assert, check_err};
use sdk::types::{ContractAddress, StorageKey};
use sdk::contract::{Contract, ContractWithStorage};
use sdk::macros::contract;

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

impl CoinContract
{
    // A helper function
    pub fn do_transfer(&self, from: &ContractAddress, to: &ContractAddress, amount: i64)
        -> Result<()>
    {
        assert(amount >= 0, "transfer amount is non-negative")?;

        self.storage().balances.get(from)?
            .add(-amount as i128)?;
        self.storage().balances.get(to)?
            .add(amount as i128)
    }
}

#[contract(export)]
impl ERC20 for CoinContract
{
    fn initialize(&mut self, owner: &ContractAddress) {
        let storage = check_err(|| {
            initialization_guard()?;
            CoinStorage::new(self.storage_key(), owner)
        });
        self.set_storage(storage)
    }

    fn transfer(&mut self, to: &ContractAddress, amount: i64) {
        check_err(|| {
            let sender = sender()?;
            self.do_transfer(&sender, &to, amount)
        })
    }

    fn transfer_from(&mut self,
                     owner: &ContractAddress,
                     to: &ContractAddress,
                     amount: i64) {
        check_err(|| {
            // XX: Check amount is non-negative?

            let sender = sender()?;
            let storage = self.storage();
            storage.allowance_for(owner, &sender)?
                .add(-amount as i128)?;
            self.do_transfer(owner, to, amount)
        })
    }

    fn adjust_allowance(&mut self, spender: &ContractAddress, amount: i64) {
        check_err(|| {
            let sender = sender()?;
            self.storage().allowance_for(&sender, spender)?.add(amount as i128)
        })
    }

    fn get_balance_of(&self, addr: &ContractAddress) -> i64 {
        check_err(|| {
            self.storage().balance_of(addr)?.get()
        }) as i64
    }

    fn get_balance(&self) -> i64 {
        check_err(|| {
            let sender = sender()?;
            self.storage().balance_of(&sender)?.get()
        }) as i64
    }

    fn mint(&mut self, to: &ContractAddress, amount: i64) {
        check_err(|| {
            let storage = self.storage();
            storage.assert_sender_is_owner()?;

            assert(amount >= 0, "mint amount is nonnegative")?;

            let mut balance = storage.balance_of(to)?;
            balance.add(amount as i128)
        })
    }

    fn burn(&mut self, to: &ContractAddress, amount: i64) {
        check_err(|| {
            let storage = self.storage();
            storage.assert_sender_is_owner()?;

            assert(!(amount > 0 || amount == i64::MIN), "burn amount is negative")?;

            storage.balance_of(to)?.add(-amount as i128)
        })
    }
}
