#![allow(non_snake_case)]
#![allow(unreachable_patterns)]

pub use crate::types::SHA256Hash;

use crate::result::SysResult as Result;
use crate::sys;
use crate::types::{PublicKey, Signature};

pub fn SHA256(input: &[u8]) -> Result<SHA256Hash> {
    let mut out_hash = core::mem::MaybeUninit::<SHA256Hash>::uninit();
    sys::uninit_SHA256(input, &mut out_hash)?;
    let hash = unsafe { out_hash.assume_init() };
    Ok(hash)
}

pub fn VerifySignature(
    pk: &PublicKey,
    signature: &Signature,
    message: &[u8],
) -> Result<()> {
    sys::uninit_VerifySignature(signature, pk, message)
}
