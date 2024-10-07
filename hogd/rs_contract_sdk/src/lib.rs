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

pub mod bytestring;
pub mod crypto;
pub mod syscall;

pub mod syscall_bindings;

pub mod rt;

pub use rs_xdr::*;
