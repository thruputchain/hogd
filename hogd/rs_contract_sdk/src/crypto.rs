#![allow(non_snake_case)]

use crate::syscall_bindings::uninit_SHA256;
use core::mem::MaybeUninit;
use rs_xdr::SHA256Hash;

pub fn SHA256(input: &[u8]) -> Result<SHA256Hash, i32> {
    let mut out_hash: MaybeUninit<SHA256Hash> =
        MaybeUninit::<SHA256Hash>::uninit();

    match uninit_SHA256(input, &mut out_hash) {
        Ok(()) => Ok(unsafe { out_hash.assume_init() }),
        Err(x) => Err(x),
    }
}
