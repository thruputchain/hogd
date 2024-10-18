#![no_std]
#![feature(maybe_uninit_slice)]

use rs_contract_sdk as sdk;

use rs_macros::{public_interface, public_method};

extern crate alloc;
use core::mem::MaybeUninit;

public_interface!();

// Has no replay prevention in this version

const PUBLIC_KEY: sdk::StorageKey = sdk::StaticKey(1234);
const SEMAPHORE_KEY: sdk::StorageKey = sdk::StaticKey(1235);
const CONFIG_LEN: usize = core::mem::size_of::<sdk::ContractAddress>()
    + core::mem::size_of::<sdk::Ed25519PublicKey>();

fn initialization_guard() {
    match sdk::HasKey(&PUBLIC_KEY) {
        Err(_) => {
            sdk::rt::trap();
        }
        _ => (),
    };
}

fn write_config_data(erc20_addr: &sdk::ContractAddress, pk: &sdk::Ed25519PublicKey) {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); CONFIG_LEN];
    sdk::parse_trait::serialize_to_calldata_2(&mut bytes_mu, pk, erc20_addr)
        .unwrap();
    let bytes: [u8; CONFIG_LEN] = unsafe { core::mem::transmute(bytes_mu) };

    sdk::BytestringSet(&PUBLIC_KEY, &bytes).unwrap();
}

fn load_config_data() -> (sdk::ContractAddress, sdk::Ed25519PublicKey) {
    // TODO(geoff) see marginal impact of rewriting without allocator
    let config = sdk::BytestringGet(&PUBLIC_KEY).unwrap();

    sdk::parse_trait::parse_returns_from_calldata::<(
        sdk::ContractAddress,
        sdk::Ed25519PublicKey,
    )>(&config)
    .unwrap()
}

#[public_method]
pub fn initialize(
    pk: &sdk::Ed25519PublicKey,
    erc20_addr: &sdk::ContractAddress,
) -> () {
    sdk::NonnegativeInt64SetAdd(&SEMAPHORE_KEY, 1, -1).unwrap();
    sdk::DeleteLast(&SEMAPHORE_KEY).unwrap();

    initialization_guard();

    write_config_data(&erc20_addr, &pk);
}

// no auth on this method
#[public_method]
pub fn get_balance() -> i64 {
    let (erc20addr, _) = load_config_data();
    let token = erc20::erc20::new(&erc20addr);

    token.get_balance().unwrap()
}

/*
#[public_method]
pub fn send_payment(to_addr: &sdk::ContractAddress, amount: i64) {
    let (erc20addr, pk) = load_config_data();

    let token = erc20::erc20::new(&erc20addr);
} */
