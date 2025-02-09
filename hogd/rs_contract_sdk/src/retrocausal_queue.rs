#![allow(non_snake_case)]

use crate::sys;
use crate::types::{RuntimeMemoryLength, StorageKey};
use crate::result::SysResult as Result;

use core::mem::MaybeUninit;

pub fn RetrocausalQueueInsert(
    key: &StorageKey,
    queue_entry: &[u8],
) -> Result<()> {
    sys::raw_RetrocausalQueueInsert(key, queue_entry)
}

pub fn RetrocausalQueueConsume_Sized<const N: usize>(
    key: &StorageKey,
) -> Result<[u8; N]> {
    let mut queue_entry = [MaybeUninit::uninit(); N];
    let mut written_len: RuntimeMemoryLength = 0;
    sys::uninit_RetrocausalQueueConsume(
        key,
        false,
        &mut queue_entry,
        &mut written_len,
    )?;
    Ok(unsafe { MaybeUninit::array_assume_init(queue_entry) })
}

pub fn RetrocausalQueuePeek_Sized<const N: usize>(
    key: &StorageKey,
) -> Result<[u8; N]> {
    let mut queue_entry = [MaybeUninit::uninit(); N];
    let mut written_len: RuntimeMemoryLength = 0;
    sys::uninit_RetrocausalQueueConsume(
        key,
        true,
        &mut queue_entry,
        &mut written_len,
    )?;
    Ok(unsafe { MaybeUninit::array_assume_init(queue_entry) })
}
