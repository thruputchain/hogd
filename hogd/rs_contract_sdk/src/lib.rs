#![cfg_attr(not(feature = "std"), no_std)]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]

#[cfg(any(feature = "alloc", feature = "std"))]
extern crate alloc;

#[cfg(all(feature = "alloc", target_family = "wasm"))]
pub mod allocator;

pub use rs_xdr as types;

pub extern crate macros;

mod rt;
mod syscall;

#[rustfmt::skip]
pub mod syscall_bindings;
pub use syscall_bindings as sys;

mod bytestring;
mod delete;
mod nonnegative_int;
mod retrocausal_queue;

pub use initialize::*;
pub use bytestring::*;
pub use delete::*;
pub use nonnegative_int::*;
pub use retrocausal_queue::*;

pub mod initialize;
pub mod invoke;
pub use invoke::*;
pub mod result;
pub mod contract;
pub mod deploy;
pub mod storage;
pub mod io;
pub mod crypto;
pub mod log;

/*
pub use types::*;
pub use contract::*;
pub use crypto::*;
pub use log::*;
*/
