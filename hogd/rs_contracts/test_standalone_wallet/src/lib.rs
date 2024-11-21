#![no_std]
#![feature(maybe_uninit_slice)]

use rs_contract_sdk as sdk;

use sdk::rt::trap;

use rs_macros::{public_interface, public_method, static_key, verify_auth};

public_interface!();

const CONFIG_KEY: sdk::StorageKey = static_key!("config key");
const CONFIG_LEN: usize = core::mem::size_of::<sdk::Ed25519PublicKey>();

const REPLAY_CACHE_KEY: sdk::StorageKey = static_key!("nonce replay cache");

fn write_config_data(pk: &sdk::Ed25519PublicKey) {
    sdk::BytestringSet(&CONFIG_KEY, &pk).unwrap();
}

fn load_config_data() -> (sdk::Ed25519PublicKey) {
    // TODO(geoff) see marginal impact of rewriting without allocator
    let config = sdk::BytestringGet(&CONFIG_KEY).unwrap();
    config
}

#[verify_auth]
pub fn verify_auth_handler(args: sdk::VerifyAuthArgs) -> () {
    let pk = load_config_data();

    if args.witness_count != 1 {
        trap();
    }

    let signature: sdk::Ed25519Signature =
        sdk::GetAuthWitness_Sized::<64>(0).unwrap();

    let auth_message: sdk::SHA256Hash =
        sdk::GetDefaultAuthPayloadHash().unwrap();

    sdk::VerifySignatureEd25519(&pk, &signature, &auth_message).unwrap();
}
