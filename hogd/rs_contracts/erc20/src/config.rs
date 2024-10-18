use rs_contract_sdk as sdk;
use core::mem::MaybeUninit;

use rs_macros::static_key;

const CONFIG_KEY: sdk::StorageKey = static_key!("config key");

pub fn write_config_data(
    owner: &sdk::ContractAddress
) {
    const CONFIG_LEN : usize = 32;
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); CONFIG_LEN];
    sdk::parse_trait::serialize_to_calldata_1(&mut bytes_mu, owner)
        .unwrap();
    let bytes: [u8; CONFIG_LEN] = unsafe { core::mem::transmute(bytes_mu) };

    sdk::BytestringSet(&CONFIG_KEY, &bytes).unwrap();
}

pub fn load_config_data() -> sdk::ContractAddress {
    const CONFIG_LEN : usize = 32;

    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); CONFIG_LEN];
    let mut len : sdk::RuntimeMemoryLength = 0;

    sdk::syscall_bindings::uninit_BytestringGet(&CONFIG_KEY, &mut bytes_mu, &mut len).unwrap();

    if len != CONFIG_LEN as u32 {
        sdk::rt::trap();
    }
    let bytes : [u8; CONFIG_LEN] = unsafe {core::mem::transmute(bytes_mu)};

    sdk::parse_trait::parse_returns_from_calldata(&bytes).unwrap()
}
