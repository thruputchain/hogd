#![allow(non_snake_case)]
#![allow(unreachable_patterns)]

pub use crate::types::SHA256Hash;

#[cfg(target_family = "wasm")]
pub fn SHA256(input: &[u8]) -> Result<SHA256Hash, i32> {
    let mut out_hash = core::mem::MaybeUninit::<SHA256Hash>::uninit();
    match crate::sys::uninit_SHA256(input, &mut out_hash) {
        Ok(()) => Ok(unsafe { out_hash.assume_init() }),
        Err(x) => Err(x),
    }
}

#[cfg(not(target_family = "wasm"))]
use sha2::{Digest, Sha256};
#[cfg(not(target_family = "wasm"))]
pub fn SHA256(input: &[u8]) -> Result<SHA256Hash, i32> {
    Ok(Sha256::digest(input).into())
}

use crate::syscall::to_offset;
use crate::types::{PublicKey, Signature, SyscallErrorCode};

pub fn VerifySignature(
    pk: &PublicKey,
    signature: &Signature,
    message: &[u8],
) -> Result<(), i32>
{
    if let (PublicKey::Ed25519 { ed25519: pk_local },
            Signature::Ed25519 { ed25519: sig_local }) = (pk, signature)
    {
        uninit_VerifySignature(
            signature.scheme() as u64,
            to_offset(sig_local) as u64,
            pk.scheme() as u64,
            to_offset(pk_local) as u64,
            message
            )
    } else {
        Err(SyscallErrorCode::kInvalidInput as i32)
    }
}
