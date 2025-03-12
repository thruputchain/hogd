#![no_std]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]
#![doc = include_str!("../README.md")]
#![doc(html_logo_url = "../../../etc/groundhog.png")]

#[cfg(feature = "std")]
extern crate std;

#[cfg(feature = "alloc")]
extern crate alloc;

#[cfg(all(feature = "alloc", target_family = "wasm"))]
pub mod allocator;

#[doc(inline)]
pub use ::macros;

pub mod serialize {
    pub use ::bcs::*;
    pub use ::serde::{de::DeserializeOwned, Deserialize, Serialize};
}

mod rt;

#[cfg(not(feature = "mock"))]
mod custom_arg;

pub mod bytestring;
pub mod contract;
pub mod crypto;
pub mod delete;
pub mod deploy;
pub mod initialize;
pub mod invoke;
pub mod log;
pub mod nonnegative_int;
pub mod result;
pub mod retrocausal_queue;
pub mod syscall;
#[doc(inline)]
pub use syscall::bindings as sys;
pub mod types;

#[cfg(feature = "alloc")]
pub mod internal;

#[cfg(feature = "alloc")]
pub mod storage;

#[cfg(feature = "mock")]
pub use ::groundhog_xdr::ffi_enums::*;

#[cfg(feature = "mock")]
pub use ::native_exec::*;

pub mod prelude {
    #[doc(inline)]
    pub use super::bytestring::*;

    #[doc(inline)]
    pub use super::contract::ContractStorage;

    #[doc(inline)]
    pub use super::deploy::*;

    #[doc(inline)]
    pub use super::initialize::initialization_guard;

    #[doc(inline)]
    pub use super::invoke::{GetCurrentAddress, GetMsgSender};

    #[doc(inline)]
    pub use super::log::Log;

    #[doc(inline)]
    pub use super::macros::contract;

    #[doc(inline)]
    pub use super::nonnegative_int::*;

    #[doc(inline)]
    pub use super::result::{ensure, Result};

    #[doc(inline)]
    pub use super::retrocausal_queue::*;

    #[doc(inline)]
    pub use super::serialize::{self, Deserialize, Serialize};

    #[doc(inline)]
    pub use super::syscall::SyscallErrorCode;

    #[doc(inline)]
    pub use super::types::{
        ByteArray, ContractAddress, Ed25519PublicKey, Ed25519Signature,
        PublicKey, SHA256Hash, Signature, StorageKey,
    };

    #[doc(inline)]
    #[cfg(feature = "alloc")]
    pub use super::storage::*;

    #[doc(inline)]
    #[cfg(feature = "alloc")]
    pub use alloc::{vec, vec::Vec};
}
