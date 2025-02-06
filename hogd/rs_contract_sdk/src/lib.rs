#![cfg_attr(not(feature = "std"), no_std)]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]

#[cfg(feature = "alloc")]
pub mod alloc;

mod syscall;

#[rustfmt::skip]
pub mod syscall_bindings;

mod bytestring;
mod contract;
mod crypto;
mod delete;
mod invoke;
mod log;
mod nonnegative_int;
mod retrocausal_queue;

pub mod parse_trait;
pub mod rt;

pub use bytestring::*;
pub use contract::*;
pub use crypto::*;
pub use delete::*;
pub use invoke::*;
pub use log::*;
pub use nonnegative_int::*;
pub use retrocausal_queue::*;
pub use rs_xdr::*;
