#![no_std]

use rs_contract_sdk as sdk;
use rs_macros::{public_interface, public_method, static_key};

extern crate alloc;

const KEY1: sdk::StorageKey = static_key!("queue key");
const KEY2: sdk::StorageKey = static_key!("queue key2");
public_interface!();

#[public_method(static_calldata_length = 0, static_return_length = 0)]
pub fn test_read_self_item() {
    sdk::RetrocausalQueueInsert(&KEY1, &[1]).unwrap();
    sdk::RetrocausalQueueInsert(&KEY1, &[2]).unwrap();

    let item = sdk::RetrocausalQueuePeek_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 1);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 1);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 2);

    match sdk::RetrocausalQueuePeek_Sized::<1>(&KEY1) {
        Ok(_) => {
            sdk::rt::trap();
        }
        Err(_) => {}
    }
}

#[public_method]
pub fn make_several_inserts(inserts: &[u8]) {
    for i in inserts {
        sdk::RetrocausalQueueInsert(&KEY1, &[*i]).unwrap();
    }
}

#[public_method]
pub fn expect_several_consumes(expected: &[u8]) {
    for i in expected {
        let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
        assert_eq!(item[0], *i);
    }
}

#[public_method]
pub fn prep_for_test_read_weird_order() {
    sdk::RetrocausalQueueInsert(&KEY1, &[1]).unwrap();
    sdk::RetrocausalQueueInsert(&KEY1, &[2]).unwrap();
    sdk::RetrocausalQueueInsert(&KEY1, &[3]).unwrap();
    sdk::RetrocausalQueueInsert(&KEY2, &[4]).unwrap();
    sdk::RetrocausalQueueInsert(&KEY2, &[5]).unwrap();
}

#[public_method]
pub fn test_read_weird_order() {
    // Expected ndet proposal queue orders:
    // Key1: 1, 2, 3
    // Key2: 4, 6
    // Key2 inserts a 5 after reading the 4.

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 1);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 2);

    let item = sdk::RetrocausalQueuePeek_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 4);

    sdk::RetrocausalQueueInsert(&KEY2, &[6]).unwrap();

    let item = sdk::RetrocausalQueuePeek_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 3);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 3);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 4);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 5);

    let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 6);
}
