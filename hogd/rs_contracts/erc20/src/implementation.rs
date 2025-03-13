//! An implementation of ERC20

// If the interface's `Amount` type changes away from `i128`, our casts
// below will be necessary
#![allow(clippy::unnecessary_cast)]

use super::interface::{Amount, ERC20};
use sdk::prelude::*;

type Balance = NNInt;
type Allowance = NNInt;

#[derive(Contract, Debug, Serialize)]
pub struct CoinContract {
    contract_owner: ContractAddress,
    balances: Map<ContractAddress, Balance>,
    allowances: Map<ContractAddress, Map<ContractAddress, Allowance>>,
    total_supply: NNInt,
}

// This provides a default key for this root object
impl ContractStorage for CoinContract {}

impl CoinContract {
    // These internal functions use the underlying storage to
    // create an abstraction whereby all balances and allowances
    // exist, defaulting to zero if never set.  They also, via
    // the NNInt datatype, enforce the invariant that balances
    // and allowances are non-negative.  They do _not_ check
    // authorization.

    fn _contract_owner(&self) -> &ContractAddress {
        &self.contract_owner
    }

    // Balance stored at given owner address, or zero if empty
    fn _balance_amount_of(&self, owner: &ContractAddress) -> Result<Amount> {
        let balance = self.balances.get(owner)?;
        let amount = balance.try_get_current()?.unwrap_or(0) as Amount;
        Ok(amount)
    }

    // Adjust balance at given owner address, from zero if empty
    fn _adjust_balance(
        &mut self,
        owner: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let mut balance = self.balances.get(owner)?;

        // The NNInt datatype ensures that `add` with a negative
        // number results in a non-negative value.
        // For never-stored-to addresses, it acts as
        // if the address had contained (0, 0).
        balance.add(amount as i128)?;
        Ok(())
    }

    // Adjust allowance for owner and spender, from zero if empty
    fn _adjust_allowance(
        &mut self,
        owner: &ContractAddress,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        #[allow(unused_mut)] // TODO
        let mut owner_allowances = self.allowances.get(owner)?;
        let mut spender_allowance = owner_allowances.get(spender)?;
        spender_allowance.add(amount as i128)?;
        Ok(())
    }

    fn _adjust_total_supply(&mut self, amount: Amount) -> Result<()> {
        self.total_supply.add(amount as i128)?;
        Ok(())
    }

    fn _total_supply(&self) -> Result<Amount> {
        let amount =
            self.total_supply.try_get_current()?.unwrap_or(0) as Amount;
        Ok(amount)
    }
}

// This implements the ERC20 interface.  The `contract` macro provides
// an exported entrypoint for each method; these will be placed in a
// module with the specified name ("guest_exports").
#[contract(exports = "guest_exports")]
impl ERC20 for CoinContract {
    fn initialize(
        _maybe_self: Option<&mut Self>,
        contract_owner: &ContractAddress,
    ) -> Result<()> {
        let keypath = Self::keypath();
        let storage_key = keypath.generate_key();

        // Make sure we haven't already been initialized
        if bytestring_exists(&storage_key)? {
            panic!("Contract already initialized");
        }

        let storage = Self {
            contract_owner: *contract_owner,
            balances: Map::new(&keypath),
            allowances: Map::new(&keypath),
            total_supply: NNInt::new(&keypath),
        };

        // Store ourselves
        store(&storage_key, &storage)?;

        Ok(())
    }

    fn transfer(
        &mut self,
        receiver: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
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
        self._balance_amount_of(owner)
    }

    fn get_balance(&self) -> Result<Amount> {
        let owner = GetMsgSender()?;
        self._balance_amount_of(&owner)
    }

    fn mint(
        &mut self,
        receiver: &ContractAddress,
        amount: Amount,
    ) -> Result<()> {
        let caller = GetMsgSender()?;
        ensure(
            &caller == self._contract_owner(),
            "Minter must be contract owner",
        )?;
        ensure(amount >= 0, "Mint amount must be non-negative")?;

        self._adjust_balance(receiver, amount)?;
        self._adjust_total_supply(amount)
    }

    fn burn(&mut self, owner: &ContractAddress, amount: Amount) -> Result<()> {
        let caller = GetMsgSender()?;
        ensure(
            &caller == self._contract_owner(),
            "Burner must be contract owner",
        )?;
        ensure(amount >= 0, "Burn amount must be non-negative")?;

        self._adjust_balance(owner, -amount)?;
        self._adjust_total_supply(-amount)
    }

    fn total_supply(&self) -> Result<Amount> {
        self._total_supply()
    }
}
