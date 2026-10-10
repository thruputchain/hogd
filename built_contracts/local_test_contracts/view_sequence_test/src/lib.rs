//! Invocation simulation fixtures, not a production financial contract.
#![no_std]

use groundhog_erc20_if::ERC20Client;
use sdk::*;

#[contract(client = Client)]
pub trait ViewSequenceTest {
    fn swap(
        input: &ContractAddress,
        collateral: &ContractAddress,
        amount: i128,
    );
    fn borrow(
        collateral: &ContractAddress,
        debt_token: &ContractAddress,
        amount: i128,
    );
    fn debt(owner: &ContractAddress) -> i128;
    fn check_source_claim(required_weight: u32, allowed_keys: u32);
    fn check_pubkey_claim(key: &[u8; 32], nonce: u64);
    fn check_nested_source_claim(
        target: &ContractAddress,
        required_weight: u32,
        allowed_keys: u32,
    );
}

pub struct Sequence;

fn debt_key(owner: &ContractAddress) -> StorageKey {
    (b"debt", owner).hash().into()
}

#[contract]
impl ViewSequenceTest for Sequence {
    fn swap(
        input: &ContractAddress,
        collateral: &ContractAddress,
        amount: i128,
    ) {
        assert!(amount > 0);
        let caller = get_caller();
        ERC20Client::new(input).transfer_from(
            &caller,
            &get_current_address(),
            amount,
        );
        ERC20Client::new(collateral).transfer(&caller, amount);
    }

    fn borrow(
        collateral: &ContractAddress,
        debt_token: &ContractAddress,
        amount: i128,
    ) {
        assert!(amount > 0);
        let caller = get_caller();
        ERC20Client::new(collateral).transfer_from(
            &caller,
            &get_current_address(),
            amount,
        );
        non_negative_int_inc(&debt_key(&caller), amount);
        ERC20Client::new(debt_token).transfer(&caller, amount);
    }

    fn debt(owner: &ContractAddress) -> i128 {
        non_negative_int_try_get(&debt_key(owner))
            .none_if_missing()
            .unwrap_or(0)
    }

    fn check_source_claim(required_weight: u32, allowed_keys: u32) {
        match get_auth_claim().expect("claim syscall") {
            AuthClaim::kSourceAddressAuthData {
                source_address_claim,
            } => {
                assert_eq!(
                    source_address_claim.required_weight,
                    required_weight
                );
                assert_eq!(source_address_claim.allowed_keys, allowed_keys);
            }
            _ => panic!("expected source-address claim"),
        }
    }

    fn check_pubkey_claim(key: &[u8; 32], nonce: u64) {
        match get_auth_claim().expect("claim syscall") {
            AuthClaim::kPubkeySignature { pubkey_preimage } => {
                assert_eq!(
                    pubkey_preimage.pubkey,
                    PublicKey::Ed25519 {
                        ed25519: Ed25519PublicKey::from(*key)
                    }
                );
                assert_eq!(pubkey_preimage.nonce, nonce);
            }
            _ => panic!("expected pubkey claim"),
        }
    }

    fn check_nested_source_claim(
        target: &ContractAddress,
        required_weight: u32,
        allowed_keys: u32,
    ) {
        Client::new(target).check_source_claim(required_weight, allowed_keys);
    }
}
