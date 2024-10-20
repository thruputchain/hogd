use core::mem::MaybeUninit;
use rs_contract_sdk as sdk;

use rs_macros::static_key;

const CONFIG_KEY: sdk::StorageKey = static_key!("config key");
const CONFIG_LEN: usize = 32;

pub fn write_config_data(owner: &sdk::ContractAddress) {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); CONFIG_LEN];
    sdk::parse_trait::serialize_to_calldata_1(&mut bytes_mu, owner).unwrap();
    let bytes: [u8; CONFIG_LEN] = unsafe { core::mem::transmute(bytes_mu) };

    sdk::BytestringSet(&CONFIG_KEY, &bytes).unwrap();
}

pub fn load_config_data() -> sdk::ContractAddress {
    let bytes = sdk::BytestringGet_Sized::<CONFIG_LEN>(&CONFIG_KEY).unwrap();
    sdk::parse_trait::parse_returns_from_calldata(&bytes).unwrap()
}
