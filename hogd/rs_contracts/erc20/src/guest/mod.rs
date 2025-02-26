//! An implementation of ERC20

mod storage;

use core::ops::DerefMut;
use sdk::contract::{Contract, ContractWithStorage};
use sdk::initialize::initialization_guard;
use sdk::macros::contract;
use sdk::result::{ensure, Result};
use core::ops::Deref;
use sdk::storage::Keypath;
use sdk::types::{ContractAddress, StorageKey};
use sdk::GetMsgSender;
use super::interface::{Amount, ERC20};
use storage::CoinStorage;

/// A wrapped contract using `CoinStorage` as persistent state
pub struct CoinContract(ContractWithStorage<CoinStorage>);

// Forward method calls to wrapped object
impl Deref for CoinContract {
    type Target = ContractWithStorage<CoinStorage>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

// Forward method calls to wrapped object
impl DerefMut for CoinContract {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl Contract for CoinContract {
    fn new(key: &StorageKey) -> Self {
        Self(ContractWithStorage::new(key))
    }
}

// Internal functions that use the underlying storage to
// create an abstraction whereby all balances and allowances
// exist, defaulting to zero if never set.  They also, via
// the NNInt datatype, enforce the invariant that balances
// and allowances are non-negative.  They do _not_ check
// authorization.
impl CoinContract {
    pub fn _initialize_storage(&mut self, owner: &ContractAddress) -> Result<()> {
        let keypath = Keypath::from_storage_key(self.storage_key());
        let storage = CoinStorage::new(&keypath, owner)?;
        self.set_storage(storage);
        Ok(())
    }

    pub fn _contract_owner(&self) -> &ContractAddress {
        &self.storage().contract_owner()
    }

    pub fn _balance_amount_of(&self, address: &ContractAddress) -> Result<Amount> {
        let balance = self.storage().balance_of(address)?;
        let amount = balance.get_current()? as Amount;
        Ok(amount)
    }

    pub fn _adjust_balance(&mut self, owner: &ContractAddress, amount: Amount) -> Result<()> {
        let mut balance = self.storage().balance_of(owner)?;

        // The NNInt datatype ensures that `add` with a negative
        // number results in a non-negative value.
        // For never-stored-to addresses, it acts (XXX?) as
        // if the address contained (0, 0).
        balance.add(amount as i128)?;
        Ok(())
    }

    pub fn _adjust_allowance(
        &mut self,
        owner: &ContractAddress,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let mut allowance = self.storage().allowance_for(owner, spender)?;

        // The NNInt datatype ensures that `add` with a negative
        // number results in a non-negative value
        allowance.add(amount as i128)?;
        Ok(())
    }

    pub fn _adjust_total_supply(&mut self, amount: Amount) -> Result<()> {
        self.storage_mut().total_supply.add(amount as i128)?;
        Ok(())
    }
}

#[contract(mod_name = "exports")]
impl ERC20 for CoinContract {
    fn initialize(&mut self, owner: &ContractAddress) -> Result<()> {
        initialization_guard()?;

        self._initialize_storage(owner)
    }

    fn transfer(&mut self, receiver: &ContractAddress, amount: Amount) -> Result<()> {
        let owner = GetMsgSender()?;

        self._adjust_balance(&owner, -amount)?;
        self._adjust_balance(receiver, amount)
    }

    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        receiver: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        ensure(amount >= 0, "Transfer amount must be positive")?;

        let spender = GetMsgSender()?;
        self._adjust_allowance(owner, &spender, -amount)?;

        self._adjust_balance(owner, -amount)?;
        self._adjust_balance(receiver, amount)
    }

    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let owner = GetMsgSender()?;
        self._adjust_allowance(&owner, spender, amount)
    }

    fn get_balance_of(&self, owner: &ContractAddress) -> Result<Amount> {
        self._balance_amount_of(&owner)
    }

    fn get_balance(&self) -> Result<Amount> {
        let owner = GetMsgSender()?;
        self._balance_amount_of(&owner)
    }

    fn mint(&mut self, receiver: &ContractAddress, amount: Amount) -> Result<()> {
        let caller = GetMsgSender()?;
        ensure(&caller == self._contract_owner(), "Minter must be contract owner")?;
        ensure(amount >= 0, "Mint amount must be non-negative")?;

        self._adjust_balance(receiver, amount)?;
        self._adjust_total_supply(amount)
    }

    fn burn(&mut self, owner: &ContractAddress, amount: Amount) -> Result<()> {
        let caller = GetMsgSender()?;
        ensure(&caller == self._contract_owner(), "Burner must be contract owner")?;
        ensure(amount >= 0, "Burn amount must be non-negative")?;

        self._adjust_balance(owner, amount)?;
        self._adjust_total_supply(-amount)
    }
}
