#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::{RuntimeMemoryLength, StorageKey};

use core::mem::MaybeUninit;

pub fn RetrocausalQueueInsert(
    key: &StorageKey,
    queue_entry: &[u8],
) -> Result<(), i32> {
    syscall_bindings::raw_RetrocausalQueueInsert(key, queue_entry)
}

pub fn RetrocausalQueueConsume_Sized<const N: usize>(
    key: &StorageKey,
) -> Result<[u8; N], i32> {
    let mut queue_entry = [MaybeUninit::uninit(); N];
    let mut written_len: RuntimeMemoryLength = 0;
    syscall_bindings::uninit_RetrocausalQueueConsume(
        key,
        false,
        &mut queue_entry,
        &mut written_len,
    )?;
    Ok(unsafe { MaybeUninit::array_assume_init(queue_entry) })
}
