#![no_std]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]


#[cfg(feature = "alloc")]
pub mod alloc;

mod syscall;

#[rustfmt::skip]
pub mod syscall_bindings;

mod nonnegative_int;
mod bytestring;
mod crypto;
mod invoke;

pub mod rt;

pub use nonnegative_int::*;
pub use bytestring::*;
pub use crypto::*;
pub use invoke::*;

pub use rs_xdr::*;