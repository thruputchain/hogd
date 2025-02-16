//! An implementation of ERC20

mod config;
use config::ConfigStorage;

use super::interface::{Amount, ERC20};
use sdk::contract::{Contract, ContractWithStorage};
use sdk::initialize::initialization_guard;
use sdk::macros::contract;
use sdk::result::{ensure, with_context, Result};
use sdk::storage::Hash;
use sdk::syscall::SyscallErrorCode;
use sdk::types::{ContractAddress, StorageKey};
use sdk::GetMsgSender;

pub struct CoinContract(ContractWithStorage<ConfigStorage>);

use core::ops::Deref;
impl Deref for CoinContract {
    type Target = ContractWithStorage<ConfigStorage>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Contract for CoinContract {
    fn new(key: &StorageKey) -> Self {
        Self(ContractWithStorage::new(key))
    }
}

fn get_balance_key(addr: &ContractAddress) -> StorageKey {
    let item_key_info = (b"balance", addr);
    let item_key: StorageKey = item_key_info
        .hash()
        .map_err(with_context("hash failed"))
        .unwrap();
    item_key
}

fn get_allowance_key(
    owner: &ContractAddress,
    spender: &ContractAddress,
) -> StorageKey {
    debug_assert!(owner != spender, "no allowance needed on spending self");
    let item_key_info = (b"allowance", owner, spender);
    let item_key: StorageKey = item_key_info
        .hash()
        .map_err(with_context("hash failed"))
        .unwrap();
    item_key
}

fn add_to_balance(addr: &ContractAddress, amount: Amount) -> Result<()> {
    let balance_key = get_balance_key(addr);
    let (s, a) = match sdk::NonnegativeIntGet(&balance_key) {
        Ok((s, a)) => (s, a),
        Err(syserr) => {
            if syserr.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                (0, 0)
            } else {
                return Err(syserr.into());
            }
        }
    };
    Ok(sdk::NonnegativeIntSetAdd(&balance_key, s, a + (amount as i128))?)
}

fn add_to_allowance(
    owner: &ContractAddress,
    spender: &ContractAddress,
    amount: Amount,
) -> Result<()> {
    let key = get_allowance_key(owner, spender);
    let (s, a) = match sdk::NonnegativeIntGet(&key) {
        Ok((s, a)) => (s, a),
        Err(syserr) => {
            if syserr.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                (0, 0)
            } else {
                return Err(syserr.into());
            }
        }
    };
    Ok(sdk::NonnegativeIntSetAdd(&key, s, a + (amount as i128))?)
}

#[contract(mod_name = "exports")]
impl ERC20 for CoinContract {
    fn initialize(&mut self, owner: &ContractAddress) -> Result<()> {
        initialization_guard()?;

        let storage = ConfigStorage::new(owner)?;
        self.set_storage(storage);
        Ok(())
    }

    fn transfer(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        let sender = GetMsgSender()?;

        ensure(amount >= 0, "no negative spends allowed")?;

        add_to_balance(&sender, -amount)?;
        add_to_balance(to, amount)?;
        Ok(())
    }

    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        to: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        ensure(amount >= 0, "no negative spends allowed")?;
        let spender = GetMsgSender()?;
        add_to_allowance(owner, &spender, -amount)?;

        add_to_balance(owner, -amount)?;
        add_to_balance(to, amount)?;
        Ok(())
    }

    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let owner = GetMsgSender()?;

        add_to_allowance(&owner, spender, amount)?;
        Ok(())
    }

    fn get_balance_of(&self, addr: &ContractAddress) -> Result<Amount> {
        let balance_key = get_balance_key(addr);
        match sdk::NonnegativeIntGet(&balance_key) {
            Ok((s, a)) => Ok((s + a).try_into().unwrap()),
            Err(syserr) => {
                if syserr.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                    return Ok(0);
                }
                Err(syserr.into())
            }
        }
    }
    fn get_balance(&self) -> Result<Amount> {
        self.get_balance_of(&GetMsgSender()?)
    }
    fn mint(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        let storage = self.storage();
        storage.ensure_sender_is_owner()?;

        ensure(amount >= 0, "mint amount is nonnegative")?;

        add_to_balance(to, amount)
    }

    fn burn(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        let storage = self.storage();
        storage.ensure_sender_is_owner()?;

        ensure(amount >= 0, "burn amount is nonnegative")?;

        add_to_balance(to, -amount)
    }
}
