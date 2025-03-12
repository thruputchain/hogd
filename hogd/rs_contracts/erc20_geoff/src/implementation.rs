//! An implementation of ERC20

// If the interface's `Amount` type changes away from `i128`, our casts
// below will be necessary
#![allow(clippy::unnecessary_cast)]
#![allow(clippy::useless_conversion)]

use super::interface::{Amount, ERC20};
use sdk::prelude::*;

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct CoinContract {
    pub owner: ContractAddress,
}

impl ContractStorage for CoinContract {}

impl CoinContract {
    pub fn ensure_sender_is_owner(&self) -> Result<()> {
        let sender = GetMsgSender()?;
        ensure(sender == self.owner, "sender is owner")
    }
}

fn get_total_supply_key() -> StorageKey {
    b"total_supply".hash().unwrap()
}

fn get_balance_key(addr: &ContractAddress) -> StorageKey {
    let item_key_info = (b"balance", addr);
    let item_key: StorageKey = item_key_info.hash().unwrap();
    item_key
}

fn get_allowance_key(
    owner: &ContractAddress,
    spender: &ContractAddress,
) -> StorageKey {
    debug_assert!(owner != spender, "no allowance needed on spending self");
    let item_key_info = (b"allowance", owner, spender);
    let item_key: StorageKey = item_key_info.hash().unwrap();
    item_key
}

fn add_to_total_supply(amount: Amount) -> Result<()> {
    let key = get_total_supply_key();
    let (s, a) = match NonnegativeIntGet(&key) {
        Ok((s, a)) => (s, a),
        Err(syserr) => {
            if syserr.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                (0, 0)
            } else {
                return Err(syserr.into());
            }
        }
    };
    Ok(NonnegativeIntSetAdd(&key, s, a + (amount as i128))?)
}

fn add_to_balance(addr: &ContractAddress, amount: Amount) -> Result<()> {
    let balance_key = get_balance_key(addr);
    let (s, a) = match NonnegativeIntGet(&balance_key) {
        Ok((s, a)) => (s, a),
        Err(syserr) => {
            if syserr.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                (0, 0)
            } else {
                return Err(syserr.into());
            }
        }
    };
    Ok(NonnegativeIntSetAdd(&balance_key, s, a + (amount as i128))?)
}

fn add_to_allowance(
    owner: &ContractAddress,
    spender: &ContractAddress,
    amount: Amount,
) -> Result<()> {
    let key = get_allowance_key(owner, spender);
    let (s, a) = match NonnegativeIntGet(&key) {
        Ok((s, a)) => (s, a),
        Err(syserr) => {
            if syserr.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                (0, 0)
            } else {
                return Err(syserr.into());
            }
        }
    };
    Ok(NonnegativeIntSetAdd(&key, s, a + (amount as i128))?)
}

#[contract(exports = "exports")]
impl ERC20 for CoinContract {
    fn initialize(
        _maybe_self: Option<&mut Self>,
        owner: &ContractAddress,
    ) -> Result<()> {
        let owner = *owner;
        let value = Self { owner };
        let key = Self::keypath().into();

        // Make sure we haven't already been initialized
        if bytestring_exists(&key)? {
            panic!("Contract already initialized");
        }

        // Store ourselves
        store(&key, &value)?;

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
        match NonnegativeIntGet(&balance_key) {
            Ok((s, a)) => Ok((s + a).try_into().unwrap()),
            Err(syserr) => {
                if syserr.is_storage_key_not_exist() {
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
        self.ensure_sender_is_owner()?;

        ensure(amount >= 0, "mint amount is nonnegative")?;

        add_to_balance(to, amount)?;
        add_to_total_supply(amount)
    }

    fn burn(&mut self, to: &ContractAddress, amount: Amount) -> Result<()> {
        self.ensure_sender_is_owner()?;

        ensure(amount >= 0, "burn amount is nonnegative")?;

        add_to_balance(to, -amount)?;
        add_to_total_supply(-amount)
    }

    fn total_supply(&self) -> Result<Amount> {
        let key = get_total_supply_key();
        match NonnegativeIntGet(&key) {
            Ok((s, a)) => Ok((s + a).try_into().unwrap()),
            Err(syserr) => {
                if syserr.is_storage_key_not_exist() {
                    return Ok(0);
                }
                Err(syserr.into())
            }
        }
    }
}
