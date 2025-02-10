#![no_std]
#![feature(maybe_uninit_slice)]

extern crate alloc;

use erc20::client::ERC20Client;
use erc20::interface::{Amount, ERC20};
use sdk::contract::{Contract, ContractWithStorage};
use sdk::macros::contract;
use sdk::types::{
    ContractAddress, Ed25519PublicKey, Ed25519SignatureA, PublicKey, Signature,
    StorageKey,
};
use serde::{Deserialize, Serialize};

// TODO: Replay prevention?

#[derive(Debug, Serialize, Deserialize)]
struct WalletStorage {
    token_addr: ContractAddress,
    pk: Ed25519PublicKey,
}

struct WalletContract(ContractWithStorage<WalletStorage>);

impl core::ops::Deref for WalletContract {
    type Target = ContractWithStorage<WalletStorage>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Contract for WalletContract {
    fn new(key: &StorageKey) -> Self {
        Self(ContractWithStorage::new(key))
    }
}

type AuthMessage = [u8; 80];

#[contract(guest)]
impl WalletContract {
    fn token(&self) -> ERC20Client {
        ERC20Client::new(&self.storage().token_addr)
    }

    pub fn initialize(
        &mut self,
        pk: &Ed25519PublicKey,
        token_addr: &ContractAddress,
    ) {
        let storage = WalletStorage {
            pk: *pk,
            token_addr: *token_addr,
        };

        self.set_storage(storage);
    }

    pub fn get_balance(&self) -> Amount {
        self.token().get_balance()
    }

    pub fn test_expect_balance(&self, expect: Amount) {
        let bal = self.get_balance();

        assert_eq!(bal, expect);
    }

    fn compute_auth_message(
        &self,
        to_addr: &ContractAddress,
        amount: Amount,
    ) -> AuthMessage {
        // TODO(geoff) this will eventually need a nonce and a domain separator(?)

        let self_addr = sdk::GetCurrentAddress().unwrap();

        let input = (&self_addr, to_addr, amount);
        let buf = sdk::serialize::to_bytes(&input).unwrap();

        let bytes: [u8; 80] = buf.try_into().unwrap();

        bytes
    }

    fn check_payment_authorization(
        &self,
        to_addr: &ContractAddress,
        amount: Amount,
        signature: &Ed25519SignatureA,
    ) {
        let pk = &self.storage().pk;

        // TODO some kind of replay mechanism
        let auth_message = self.compute_auth_message(&to_addr, amount);

        sdk::crypto::VerifySignature(
            &PublicKey::Ed25519 { ed25519: *pk },
            &Signature::Ed25519 {
                ed25519: signature.as_array().clone(),
            },
            &auth_message,
        )
        .unwrap();
    }

    pub fn send_payment(
        &self,
        to_addr: &ContractAddress,
        amount: Amount,
        signature: &Ed25519SignatureA,
    ) {
        self.check_payment_authorization(to_addr, amount, signature);

        self.token().transfer(&to_addr, amount);
    }

    /*
    pub fn try_send_payment(&self,
        to_addr: &ContractAddress,
        amount: Amount,
        signature: &Ed25519SignatureA)
    {
        self.check_payment_authorization(to_addr, amount, signature);

        self.token().set_try_invoke().transfer(&to_addr, amount);
    }
    */

    pub fn mint(&self, to_addr: &ContractAddress, amount: Amount) {
        self.token().mint(&to_addr, amount);
    }
}
