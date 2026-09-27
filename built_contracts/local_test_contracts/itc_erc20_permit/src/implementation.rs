//! `itc_erc20`'s implementation, unchanged apart from formatting and the
//! `checked` feature (which the native token has no counterpart for), plus
//! `ERC20Permit`.

// If the interface's `Amount` type changes away from `i128`, our casts
// below will be necessary
#![allow(clippy::unnecessary_cast)]
#![allow(clippy::useless_conversion)]

use crate::interface::*;
use crate::permit_interface::{ERC20Permit, ERC20PermitError, PermitGrant};
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
    // Spending one's own tokens needs no allowance; reject (owner == spender)
    // as a proper contract error rather than a debug-only assert, so the
    // behavior is identical in release. (ERC20Error::Internal for now; a more
    // descriptive variant can follow in an sdk update.)
    if owner == spender {
        throw(ERC20Error::Internal);
    }

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

        log_transfer_event(&caller, to, amount);
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

        log_transfer_event(owner, receiver, amount);
    }

    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        diff: Difference,
    ) {
        let owner = get_caller();

        add_to_allowance(&owner, spender, diff);

        log_approval_event(&owner, spender, diff);
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

        log_mint_event(receiver, amount);
    }

    fn burn(&mut self, owner: &ContractAddress, amount: Amount) {
        ensure_caller_is_owner();

        let diff = amount_to_diff(amount);
        add_to_balance(owner, -diff);
        add_to_total_supply(-diff);

        log_burn_event(owner, amount);
    }

    fn total_supply(&self) -> Amount {
        non_negative_int_try_get(&get_total_supply_key())
            .none_if_missing()
            .unwrap_or(0) as Amount
    }
}

// The owner of a grant is whoever minted it: `consume` reports that address
// from the runtime, and reaching this contract at all proves the grant names
// it as consumer. Both methods consume before anything else, as the SDK's
// example permit token does, so nothing the grant says is acted on before
// the runtime has vouched for it.
#[contract(version = "0.1.0")]
impl ERC20Permit for Token {
    fn permit(&mut self, grant: Signed<PermitGrant>) {
        let (grant, owner) = grant.consume();
        let diff = amount_to_diff(grant.amount);

        add_to_allowance(&owner, &grant.spender, diff);

        log_approval_event(&owner, &grant.spender, diff);
    }

    fn transfer_from_signed(
        &mut self,
        grant: Signed<PermitGrant>,
        receiver: &ContractAddress,
    ) {
        let (grant, owner) = grant.consume();
        // The grant names who may spend it. No allowance is written or read,
        // so this is the whole authorization check.
        ensure(get_caller() == grant.spender, ERC20PermitError::NotSpender);

        let diff = amount_to_diff(grant.amount);
        add_to_balance(&owner, -diff);
        add_to_balance(receiver, diff);

        log_transfer_event(&owner, receiver, grant.amount);
    }
}
