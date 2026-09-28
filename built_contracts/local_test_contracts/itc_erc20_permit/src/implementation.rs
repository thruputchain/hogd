//! The RV32 reference for native HOG's ERC20 and ERC20Permit interfaces.

use crate::interface::*;
use crate::permit_interface::{
    ClearAllowanceGrant, ERC20Permit, ERC20PermitError, PermitGrant,
};
use sdk::signer::Signed;
use sdk::*;

// Compute keys for storing things

fn get_owner_key() -> StorageKey {
    b"owner".hash().into()
}

fn get_total_supply_key() -> StorageKey {
    b"total_supply".hash().into()
}

fn get_balance_key(addr: &ContractAddress) -> StorageKey {
    (b"balance", addr).hash().into()
}

fn get_allowance_key(
    owner: &ContractAddress,
    spender: &ContractAddress,
) -> StorageKey {
    (b"allowance", owner, spender).hash().into()
}

// Read/write permanent state

fn get_owner() -> ContractAddress {
    bytestring_get_sized(&get_owner_key()).into()
}

fn ensure_caller_is_owner() {
    let caller = get_caller();
    let owner = get_owner();

    ensure(caller == owner, ERC20Error::MustBeContractOwner)
}

fn add_to_total_supply(diff: Difference) {
    non_negative_int_inc(&get_total_supply_key(), diff)
}

#[inline(never)]
fn add_to_balance(addr: &ContractAddress, diff: Difference) {
    // The standard error contract: a balance outside the non-negative
    // range throws ERC20Error::TooLarge. try_strict_inc reports the
    // failure instead of aborting, with no extra pre-check roundtrip.
    non_negative_int_try_strict_inc(&get_balance_key(addr), diff)
        .unwrap_or_else(|_| throw(ERC20Error::TooLarge))
}

fn add_to_allowance(
    owner: &ContractAddress,
    spender: &ContractAddress,
    diff: Difference,
) {
    non_negative_int_try_strict_inc(&get_allowance_key(owner, spender), diff)
        .unwrap_or_else(|_| throw(ERC20Error::TooLarge))
}

fn clear_allowance_for(owner: &ContractAddress, spender: &ContractAddress) {
    let key = get_allowance_key(owner, spender);
    let remaining = non_negative_int_try_get(&key)
        .none_if_missing()
        .unwrap_or(0);
    delete_exclusive(&key);
    AllowanceChangeEvent::new(owner, spender, -remaining).log();
}

#[sdk::storage]
pub struct Token;

#[contract(version = "0.1.0")]
impl ERC20 for Token {
    fn initialize(&mut self, contract_owner: &ContractAddress) {
        let key = get_owner_key();

        // Make sure we haven't already been initialized
        ensure(!bytestring_exists(&key), ERC20Error::AlreadyInitialized);

        // Store ourselves
        bytestring_set(&key, contract_owner.as_ref());
    }

    fn transfer(&mut self, to: &ContractAddress, amount: Amount) {
        let caller = get_caller();
        let diff = amount_to_diff(amount);
        add_to_balance(&caller, -diff);
        add_to_balance(to, diff);

        TransferEvent::new(&caller, to, diff).log();
    }

    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        receiver: &ContractAddress,
        amount: Amount,
    ) {
        let spender = get_caller();
        let diff = amount_to_diff(amount);
        // Spending one's own tokens needs no allowance.
        if owner != &spender {
            add_to_allowance(owner, &spender, -diff);
        }
        add_to_balance(owner, -diff);
        add_to_balance(receiver, diff);

        TransferEvent::new(owner, receiver, diff).log();
    }

    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        difference: Difference,
        exclusive: bool,
    ) {
        let owner = get_caller();
        add_to_allowance(&owner, spender, difference);
        if exclusive {
            set_storage_flags(
                &get_allowance_key(&owner, spender),
                StorageDeltaFlags::ExclusiveLock,
            );
        }
        AllowanceChangeEvent::new(&owner, spender, difference).log();
    }

    fn clear_allowance(&mut self, spender: &ContractAddress) {
        clear_allowance_for(&get_caller(), spender);
    }

    fn clear_balance(&mut self) {
        let key = get_balance_key(&get_caller());
        let balance = non_negative_int_try_get(&key)
            .none_if_missing()
            .unwrap_or(0);
        ensure(balance == 0, ERC20Error::BalanceNotZero);
        delete_exclusive(&key);
    }

    fn allowance(
        &self,
        owner: &ContractAddress,
        spender: &ContractAddress,
    ) -> Amount {
        non_negative_int_try_get(&get_allowance_key(owner, spender))
            .none_if_missing()
            .unwrap_or(0) as Amount
    }

    fn get_balance_of(&self, addr: &ContractAddress) -> Amount {
        non_negative_int_try_get(&get_balance_key(addr))
            .none_if_missing()
            .unwrap_or(0) as Amount
    }

    fn get_balance(&self) -> Amount {
        self.get_balance_of(&get_caller())
    }

    fn mint(&mut self, receiver: &ContractAddress, amount: Amount) {
        ensure_caller_is_owner();

        let diff = amount_to_diff(amount);
        add_to_balance(receiver, diff);
        add_to_total_supply(diff);

        MintEvent::new(receiver, diff).log();
    }

    fn burn(&mut self, owner: &ContractAddress, amount: Amount) {
        ensure_caller_is_owner();

        let diff = amount_to_diff(amount);
        add_to_balance(owner, -diff);
        add_to_total_supply(-diff);

        BurnEvent::new(owner, diff).log();
    }

    fn total_supply(&self) -> Amount {
        non_negative_int_try_get(&get_total_supply_key())
            .none_if_missing()
            .unwrap_or(0) as Amount
    }
}

// The grant's owner is its minter, reported by `consume`. Every method
// consumes first, so nothing in the grant is acted on unverified.
#[contract(version = "0.1.0")]
impl ERC20Permit for Token {
    fn transfer_from_signed(
        &mut self,
        grant: Signed<PermitGrant>,
        receiver: &ContractAddress,
        amount: Amount,
    ) {
        let (grant, owner) = grant.consume();
        // The grant names who may spend it and how much. No allowance is
        // written or read, so this is the whole authorization check.
        ensure(get_caller() == grant.spender, ERC20PermitError::NotSpender);
        ensure(amount <= grant.amount, ERC20PermitError::ExceedsGrant);

        let diff = amount_to_diff(amount);
        add_to_balance(&owner, -diff);
        add_to_balance(receiver, diff);

        TransferEvent::new(&owner, receiver, diff).log();
    }

    fn increase_allowance_signed(&mut self, grant: Signed<PermitGrant>) {
        let (grant, owner) = grant.consume();
        let diff = amount_to_diff(grant.amount);

        add_to_allowance(&owner, &grant.spender, diff);

        AllowanceChangeEvent::new(&owner, &grant.spender, diff).log();
    }

    fn clear_allowance_signed(&mut self, grant: Signed<ClearAllowanceGrant>) {
        let (grant, owner) = grant.consume();
        clear_allowance_for(&owner, &grant.spender);
    }
}
