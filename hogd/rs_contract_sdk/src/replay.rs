#![allow(non_snake_case)]

use crate::syscall_bindings;
use rs_xdr::StorageKey;

pub fn ConsumeReplayNonce(key: &StorageKey, nonce: u64) -> Result<(), i32> {
    const DEFAULT_BLOCK_BIT_WIDTH: u8 = 40;
    let mut cur_block_number: u64 = 0;
    syscall_bindings::raw_GetBlockNumber(&mut cur_block_number)?;

    syscall_bindings::raw_ReplayCacheClear(
        key,
        cur_block_number << (64 - DEFAULT_BLOCK_BIT_WIDTH),
    )?;
    syscall_bindings::raw_ReplayCacheInsert(key, nonce)?;

    Ok(())
}
