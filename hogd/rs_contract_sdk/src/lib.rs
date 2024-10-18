#![cfg_attr(not(feature = "std"), no_std)]
#![feature(maybe_uninit_slice)]

// If we decide rebuilding core and alloc are bad,
// we can disable the error handling in the allocator
// (and the associated nonsense code)
// with the following.  As it is, rebuilding with panic_immediate_abort
// seems to give the same code size without this.
/*
#![feature(alloc_error_handler)]

#[alloc_error_handler]
pub fn handle_alloc_error(_: Layout) -> ! {
    crate::rt::trap();
}
*/

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
mod initialize;

pub mod parse_trait;

pub mod syscall_bindings;

pub mod rt;

pub use bytestring::*;
pub use contract::*;
pub use crypto::*;
pub use general_storage::*;
pub use invoke::*;
pub use log::*;
pub use nonnegative_int64::*;
pub use initialize::*;
pub use rs_xdr::*;
