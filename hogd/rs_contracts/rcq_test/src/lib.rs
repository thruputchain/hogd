#![no_std]

use rs_contract_sdk as sdk;
use rs_macros::{public_interface, public_method, static_key};

extern crate alloc;

const CONFIG_KEY: sdk::StorageKey = static_key!("queue key");

public_interface!();

#[public_method(static_calldata_length = 0, static_return_length = 0)]
pub fn test_read_self_item() {
    sdk::RetrocausalQueueInsert(&CONFIG_KEY, &[1]).unwrap();
    sdk::RetrocausalQueueInsert(&CONFIG_KEY, &[2]).unwrap();

    let item = sdk::RetrocausalQueuePeek_Sized::<1>(&CONFIG_KEY).unwrap();
    assert_eq!(item[0], 1);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&CONFIG_KEY).unwrap();
    assert_eq!(item[0], 1);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&CONFIG_KEY).unwrap();
    assert_eq!(item[0], 2);

    match sdk::RetrocausalQueuePeek_Sized::<1>(&CONFIG_KEY) {
        Ok(_) => {
            sdk::rt::trap();
        }
        Err(_) => {}
    }
}

#[public_method]
pub fn make_several_inserts(inserts: &[u8]) {
    for i in inserts {
        sdk::RetrocausalQueueInsert(&CONFIG_KEY, &[*i]).unwrap();
    }
}

#[public_method]
pub fn expect_several_consumes(expected: &[u8]) {
    for i in expected {
        let item =
            sdk::RetrocausalQueueConsume_Sized::<1>(&CONFIG_KEY).unwrap();
        assert_eq!(item[0], *i);
    }
}
