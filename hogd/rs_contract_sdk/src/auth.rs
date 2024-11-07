#![allow(non_snake_case)]

// This is a primitive story for contract authorization.
// Long term, we need a more ergonomic process. I have some thoughts on what not to do,
// but no great answers for what to do.

use crate::parse_trait::Serializable;
use crate::rt::trap;
use crate::GetMsgSender;

use core::mem::MaybeUninit;
use rs_xdr::{ContractAddress, RuntimeMemoryLength, SHA256Hash};

pub fn assert_root_tx() {
    match GetMsgSender() {
        Ok(_) => {
            trap();
        }
        Err(x) => {
            if x == rs_xdr::SyscallErrorCode::kIsRootTransaction as i32 {
                return;
            }
            trap();
        }
    }
}

pub fn GetAuthWitness_Sized<const N: usize>(
    witness_index: u32,
) -> Result<[u8; N], i32> {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); N];

    let mut len: RuntimeMemoryLength = 0;

    crate::syscall_bindings::uninit_GetAuthWitness(
        witness_index,
        &mut bytes_mu,
        &mut len,
    )?;

    if len != N as u32 {
        crate::rt::trap();
    }

    let bytes: [u8; N] = unsafe { MaybeUninit::array_assume_init(bytes_mu) };
    Ok(bytes)
}

pub fn GetDefaultAuthPayloadHash() -> Result<rs_xdr::SHA256Hash, i32> {
    let mut out_addr: MaybeUninit<SHA256Hash> =
        MaybeUninit::<SHA256Hash>::uninit();

    crate::syscall_bindings::uninit_GetAuthPayloadHash(None, &mut out_addr)?;

    Ok(unsafe { out_addr.assume_init() })
}

pub struct AuthContext {}

impl AuthContext {
    pub fn new_Sized<T: Serializable, const N: usize>(context: T) -> Self {
        let mut bytes_mu = [MaybeUninit::<u8>::uninit(); N];

        context.do_serialize(&mut bytes_mu).unwrap();

        crate::syscall_bindings::raw_PushAuthContext(&unsafe {
            MaybeUninit::array_assume_init(bytes_mu)
        })
        .unwrap();

        AuthContext {}
    }
}

impl Drop for AuthContext {
    fn drop(&mut self) {
        crate::syscall_bindings::raw_PopAuthContext().unwrap();
    }
}

pub fn RequireAuth(authorizing_address: &ContractAddress) -> Result<(), i32> {
    crate::syscall_bindings::raw_RequireAuth(authorizing_address, None)
}

// Example:
/*

// This could be avoided if either I used an allocator
// or rust had a functional constexpr system
let _auth_context =
    sdk::AuthContext::new_Sized::<
        _,
        { "AuthorizedTransfer".as_bytes().len() + 72 },
    >(("AuthorizedTransfer".as_bytes(), owner, to, amount));

*/
