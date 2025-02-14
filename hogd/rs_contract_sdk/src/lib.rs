#![cfg_attr(not(feature = "std"), no_std)]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]

#[cfg(feature = "std")]
extern crate std;

#[cfg(any(feature = "alloc", feature = "std"))]
extern crate alloc;

#[cfg(all(feature = "alloc", target_family = "wasm"))]
pub mod allocator;

pub extern crate macros;

pub use ::bcs as serialize;

mod rt;

#[rustfmt::skip]
mod syscall;

mod custom_arg;

pub use syscall::bindings as sys;

pub mod types;

mod bytestring;
mod delete;
mod nonnegative_int;
mod retrocausal_queue;

pub use bytestring::*;
pub use delete::*;
pub use initialize::*;
pub use nonnegative_int::*;
pub use retrocausal_queue::*;

pub mod initialize;
pub mod invoke;
pub use invoke::*;
pub mod contract;
pub mod crypto;
pub mod deploy;
pub mod log;
pub mod result;
pub mod storage;

#[cfg(feature = "mock")]
pub use test_ffi::*;
