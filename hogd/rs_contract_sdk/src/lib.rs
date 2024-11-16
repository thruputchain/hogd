#![cfg_attr(not(feature = "std"), no_std)]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]

/**
 * Return type of methods is generally Result<return_type, i32>
 * where the i32 is the discriminant of the SyscallErrorCode enum.
 */

#[cfg(feature = "alloc")]
pub mod alloc;

mod bytestring;
mod contract;
mod crypto;
mod general_storage;
mod initialize;
mod invoke;
mod log;
mod nonnegative_int64;
mod replay;
mod syscall;

pub mod parse_trait;

#[rustfmt::skip]
pub mod syscall_bindings;

pub mod rt;

pub use bytestring::*;
pub use contract::*;
pub use crypto::*;
pub use general_storage::*;
pub use initialize::*;
pub use invoke::*;
pub use log::*;
pub use nonnegative_int64::*;
pub use replay::*;
pub use rs_xdr::*;
