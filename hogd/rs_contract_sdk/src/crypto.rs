#![allow(non_snake_case)]

use crate::syscall_bindings::{uninit_SHA256, uninit_VerifySignature};
use core::mem::MaybeUninit;
use rs_xdr::{SHA256Hash, CryptographicScheme, Ed25519Signature, Ed25519PublicKey};

pub fn SHA256(input: &[u8]) -> Result<SHA256Hash, i32> {
    let mut out_hash: MaybeUninit<SHA256Hash> =
        MaybeUninit::<SHA256Hash>::uninit();

    match uninit_SHA256(input, &mut out_hash) {
        Ok(()) => Ok(unsafe { out_hash.assume_init() }),
        Err(x) => Err(x),
    }
}

pub fn VerifySignatureEd25519(key: &Ed25519PublicKey, sig: &Ed25519Signature, msg: &[u8]) -> Result<(), i32>
{
    uninit_VerifySignature(
        CryptographicScheme::Ed25519 as u64,
        key.as_ptr() as u64,
        CryptographicScheme::Ed25519 as u64,
        sig.as_ptr() as u64,
        msg)
}

pub fn VerifySignatureUnspecified(scheme: i32, key: &[u8], sig: &[u8], msg: &[u8]) -> Result<(), i32>
{
    uninit_VerifySignature(
        scheme as u64,
        key.as_ptr() as u64,
        scheme as u64,
        sig.as_ptr() as u64,
        msg)
}
