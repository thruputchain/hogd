#![no_std]

extern crate alloc;
use alloc::vec::Vec;

use sdk::contract::Contract;
use sdk::macros::contract;
use sdk::macros::static_key;
use sdk::types::StorageKey;

const KEY1: StorageKey = static_key!("queue key");
const KEY2: StorageKey = static_key!("queue key2");

pub struct RCQ;

impl Contract for RCQ {
    fn new(_: &StorageKey) -> Self {
        Self
    }
}

#[contract]
pub trait RCQInterface {
    fn test_read_self_item(self);
    fn make_several_inserts(self, inserts: alloc::vec::Vec<u8>);
    fn expect_several_consumes(self, expected: alloc::vec::Vec<u8>);
    fn prep_for_test_read_weird_order(self);
    fn test_read_weird_order(self);
}

#[cfg(feature = "client")]
// A client stub that provides the interface via cross-contract calls
pub use client::RCQInterfaceClient;

#[cfg(feature = "guest")]
// Contract methods (below) exposed with C ABI for running as guest
pub use exports::*;

#[cfg(feature = "guest")]
#[contract]
impl RCQInterface for RCQ {
    fn test_read_self_item(self) {
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
                panic!("!");
            }
            Err(_) => {}
        }
    }

    fn make_several_inserts(self, inserts: Vec<u8>) {
        for i in inserts {
            sdk::RetrocausalQueueInsert(&KEY1, &[i]).unwrap();
        }
    }

    fn expect_several_consumes(self, expected: Vec<u8>) {
        for i in expected {
            let item = sdk::RetrocausalQueueConsume_Sized::<1>(&KEY1).unwrap();
            assert_eq!(item[0], i);
        }
    }

    fn prep_for_test_read_weird_order(self) {
        sdk::RetrocausalQueueInsert(&KEY1, &[1]).unwrap();
        sdk::RetrocausalQueueInsert(&KEY1, &[2]).unwrap();
        sdk::RetrocausalQueueInsert(&KEY1, &[3]).unwrap();
        sdk::RetrocausalQueueInsert(&KEY2, &[4]).unwrap();
        sdk::RetrocausalQueueInsert(&KEY2, &[5]).unwrap();
    }

    fn test_read_weird_order(self) {
        // Expected ndet proposal queue orders:
        // Key1: 1, 2, 3
        // Key2: 4, 5
        // Key2 inserts a 6

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
}
