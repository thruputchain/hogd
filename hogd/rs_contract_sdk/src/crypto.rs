#![allow(non_snake_case)]

use crate::syscall::to_offset;
use crate::syscall_bindings::uninit_SHA256;
use crate::syscall_bindings::uninit_VerifySignature;
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

pub fn VerifySignature(
    pk: &rs_xdr::PublicKey,
    signature: &rs_xdr::Signature,
    message: &[u8],
) -> Result<(), i32> {
    match (pk, signature) {
        (
            ::rs_xdr::PublicKey::Ed25519 { ed25519: pk_local },
            ::rs_xdr::Signature::Ed25519 { ed25519: sig_local },
        ) => {
            return uninit_VerifySignature(
                signature.scheme() as u64,
                to_offset(sig_local) as u64,
                pk.scheme() as u64,
                to_offset(pk_local) as u64,
                message,
            );
        }
        _ => Err(::rs_xdr::SyscallErrorCode::kInvalidInput as i32),
    }
}
