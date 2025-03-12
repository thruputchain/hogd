#![no_std]

extern crate alloc;
use alloc::vec::Vec;

use sdk::macros::static_key;
use sdk::prelude::*;

const KEY1: StorageKey = static_key!("queue key");
const KEY2: StorageKey = static_key!("queue key2");

#[contract]
fn test_read_self_item() {
    RetrocausalQueueInsert(&KEY1, &[1]).unwrap();
    RetrocausalQueueInsert(&KEY1, &[2]).unwrap();

    let item = RetrocausalQueuePeek_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 1);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 1);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 2);

    match RetrocausalQueuePeek_Sized::<1>(&KEY1) {
        Ok(_) => {
            panic!("!");
        }
        Err(_) => {}
    }
}

#[contract]
fn make_several_inserts(inserts: Vec<u8>) {
    for i in inserts {
        RetrocausalQueueInsert(&KEY1, &[i]).unwrap();
    }
}

#[contract]
fn expect_several_consumes(expected: Vec<u8>) {
    for i in expected {
        let item = RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
        assert_eq!(item[0], i);
    }
}

#[contract]
fn prep_for_test_read_weird_order() {
    RetrocausalQueueInsert(&KEY1, &[1]).unwrap();
    RetrocausalQueueInsert(&KEY1, &[2]).unwrap();
    RetrocausalQueueInsert(&KEY1, &[3]).unwrap();
    RetrocausalQueueInsert(&KEY2, &[4]).unwrap();
    RetrocausalQueueInsert(&KEY2, &[5]).unwrap();
}

#[contract]
fn test_read_weird_order() {
    // Expected ndet proposal queue orders:
    // Key1: 1, 2, 3
    // Key2: 4, 5
    // Key2 inserts a 6

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 1);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 2);

    let item = RetrocausalQueuePeek_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 4);

    RetrocausalQueueInsert(&KEY2, &[6]).unwrap();

    let item = RetrocausalQueuePeek_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 3);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
    assert_eq!(item[0], 3);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 4);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 5);

    let item = RetrocausalQueueConsume_Sized::<1>(&KEY2).unwrap();
    assert_eq!(item[0], 6);
}
