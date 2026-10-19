use crate::StorageReads;
use thruput_sdk::*;

fn key(seed: u64, i: u32) -> StorageKey {
    let mut buf = [0u8; 12];
    buf[..8].copy_from_slice(&seed.to_le_bytes());
    buf[8..].copy_from_slice(&i.to_le_bytes());
    (b"storage_reads", &buf).hash().into()
}

pub struct StorageReadsContract;

#[contract]
impl StorageReads for StorageReadsContract {
    fn init() {}

    fn read_keys(seed: u64, count: u32) -> u32 {
        let mut found = 0;
        for i in 0..count {
            if non_negative_int_try_get(&key(seed, i))
                .none_if_missing()
                .unwrap_or(-1)
                >= 0
            {
                found += 1;
            }
        }
        found
    }
}
