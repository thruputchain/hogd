#![allow(non_snake_case)]
#![allow(unreachable_patterns)]

pub use crate::types::SHA256Hash;

use crate::result::SysResult as Result;
use crate::sys;
use crate::syscall::to_offset;
use crate::types::{PublicKey, Signature};

#[cfg(target_family = "wasm")]
pub fn SHA256(input: &[u8]) -> Result<SHA256Hash> {
    let mut out_hash = core::mem::MaybeUninit::<SHA256Hash>::uninit();
    sys::uninit_SHA256(input, &mut out_hash)?;
    let hash = unsafe { out_hash.assume_init() };
    Ok(hash)
}

#[cfg(not(target_family = "wasm"))]
pub fn SHA256(input: &[u8]) -> Result<SHA256Hash> {
    use sha2::{Digest, Sha256};

    Ok(Sha256::digest(input).into())
}

pub fn VerifySignature(
    pk: &PublicKey,
    signature: &Signature,
    message: &[u8],
) -> Result<()> {
    let PublicKey::Ed25519 { ed25519: pk_local } = pk;
    let Signature::Ed25519 { ed25519: sig_local } = signature;
    sys::uninit_VerifySignature(
        signature.scheme() as u64,
        to_offset(sig_local) as u64,
        pk.scheme() as u64,
        to_offset(pk_local) as u64,
        message,
    )
}
