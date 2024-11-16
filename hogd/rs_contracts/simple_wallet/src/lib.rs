#![no_std]
#![feature(maybe_uninit_slice)]

use rs_contract_sdk as sdk;

use sdk::rt::trap;

use rs_macros::{public_interface, public_method, static_key};

extern crate alloc;
use core::mem::MaybeUninit;

public_interface!();

// Has no replay prevention in this version
const CONFIG_KEY: sdk::StorageKey = static_key!("config key");

const CONFIG_LEN: usize = core::mem::size_of::<sdk::ContractAddress>()
    + core::mem::size_of::<sdk::Ed25519PublicKey>();

const REPLAY_CACHE_KEY: sdk::StorageKey = static_key!("nonce replay cache");

fn write_config_data(
    erc20_addr: &sdk::ContractAddress,
    pk: &sdk::Ed25519PublicKey,
) {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); CONFIG_LEN];
    sdk::parse_trait::serialize_to_calldata(&mut bytes_mu, (erc20_addr, pk))
        .unwrap();
    let bytes: [u8; CONFIG_LEN] = unsafe { core::mem::transmute(bytes_mu) };

    sdk::BytestringSet(&CONFIG_KEY, &bytes).unwrap();
}

fn load_config_data() -> (sdk::ContractAddress, sdk::Ed25519PublicKey) {
    // TODO(geoff) see marginal impact of rewriting without allocator
    let config = sdk::BytestringGet(&CONFIG_KEY).unwrap();

    sdk::parse_trait::parse_returns_from_calldata::<(
        sdk::ContractAddress,
        sdk::Ed25519PublicKey,
    )>(&config)
    .unwrap()
}

// no auth on this method
#[public_method]
pub fn get_balance() -> i64 {
    let (erc20addr, _) = load_config_data();
    let token = erc20::erc20::new(&erc20addr);

    token.get_balance().unwrap()
}

#[public_method]
pub fn initialize(
    pk: &sdk::Ed25519PublicKey,
    erc20_addr: &sdk::ContractAddress,
) -> () {
    sdk::initialization_guard();
    write_config_data(&erc20_addr, &pk);
}

#[public_method]
pub fn test_expect_balance(expect: i64) {
    let (erc20addr, _) = load_config_data();
    let token = erc20::erc20::new(&erc20addr);

    if expect != token.get_balance().unwrap() {
        trap();
    }
}

#[public_method]
pub fn send_payment(to_addr: &sdk::ContractAddress, amount: i64) {
    let (erc20addr, _) = load_config_data();

    let token = erc20::erc20::new(&erc20addr);

    token.transfer(&to_addr, amount).unwrap();
}

#[public_method]
pub fn mint(to_addr: &sdk::ContractAddress, amount: i64) {
    let (erc20addr, _) = load_config_data();

    let token = erc20::erc20::new(&erc20addr);

    token.mint(&to_addr, amount).unwrap();
}
