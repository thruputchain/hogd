#![cfg_attr(not(feature = "std"), no_std)]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]

#[cfg(any(feature = "alloc", feature = "std"))]
extern crate alloc;

#[cfg(all(feature = "alloc", target_family = "wasm"))]
pub mod allocator;

pub use rs_xdr as types;

pub extern crate macros;

mod syscall;
#[rustfmt::skip]
pub mod syscall_bindings;
pub use syscall_bindings as sys;
mod bytestring;

mod initialize;
mod delete;
mod invoke;
mod log;
mod nonnegative_int;
mod retrocausal_queue;
mod rt;

pub mod result;
pub mod contract;
pub mod deploy;
pub mod storage;
pub mod io;
pub mod crypto;

pub use types::*;
pub use bytestring::*;
pub use contract::*;
pub use crypto::*;
pub use delete::*;
pub use invoke::*;
pub use log::*;
pub use nonnegative_int::*;
pub use retrocausal_queue::*;
pub use initialize::*;
