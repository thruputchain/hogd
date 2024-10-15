#![no_std]

/**
 * Return type of methods is generally Result<return_type, i32>
 * where the i32 is the discriminant of the SyscallErrorCode enum.
 * I did not include an automatic conversion to the enum,
 * because that, in rust, requires a giant switch statement
 * which (1) increases code size significantly and (2) is annoying.
 */

#[cfg(feature = "alloc")]
pub mod alloc;

mod bytestring;
mod contract;
mod crypto;
mod general_storage;
mod invoke;
mod log;
mod nonnegative_int64;
mod syscall;

pub mod syscall_bindings;

pub mod rt;

pub use bytestring::*;
pub use contract::*;
pub use crypto::*;
pub use general_storage::*;
pub use invoke::*;
pub use log::*;
pub use nonnegative_int64::*;
pub use rs_xdr::*;
