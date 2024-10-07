#![no_std]

/**
 * Return type of methods is generally Result<return_type, u64>
 * where the u64 is the discriminant of the SyscallErrorCode enum.
 * I did not include an automatic conversion to the enum,
 * because that, in rust, requires a giant switch statement
 * which (1) increases code size significantly and (2) is annoying.
 */

#[cfg(feature = "alloc")]
pub mod alloc;

pub mod crypto;
pub mod syscall;
pub mod general_storage;
pub mod bytestring;

pub mod rt;



