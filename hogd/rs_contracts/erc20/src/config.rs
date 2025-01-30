use core::mem::MaybeUninit;
use rs_contract_sdk as sdk;

use rs_macros::static_key;
use sdk::parse_trait::Serializable;

const CONFIG_KEY: sdk::StorageKey = static_key!("config key");
const CONFIG_LEN: usize = 32;

pub fn write_config_data(owner: &sdk::ContractAddress) {
    // guard against double initialization
    if sdk::BytestringGet_Sized::<CONFIG_LEN>(&CONFIG_KEY).is_ok() {
        sdk::Log(b"BBBBBBB").unwrap();
        sdk::rt::trap();
    }
    sdk::Log(b"FFFFFFF").unwrap();

    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); CONFIG_LEN];
    owner.do_serialize(&mut bytes_mu).unwrap();
    sdk::Log(b"DDDDDDD").unwrap();

    let bytes: [u8; CONFIG_LEN] = unsafe { core::mem::transmute(bytes_mu) };

    sdk::BytestringSetExclusive(&CONFIG_KEY, &bytes).unwrap();
    sdk::Log(b"EEEEEEE").unwrap();
}

pub fn load_config_data() -> sdk::ContractAddress {
    let bytes = sdk::BytestringGet_Sized::<CONFIG_LEN>(&CONFIG_KEY).unwrap();
    sdk::parse_trait::parse_returns_from_calldata(&bytes).unwrap()
}
