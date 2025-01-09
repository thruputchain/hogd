#![no_std]
#![feature(maybe_uninit_slice)]
#![feature(maybe_uninit_array_assume_init)]

#[cfg(feature = "alloc")]
pub mod alloc;

mod syscall;

#[rustfmt::skip]
pub mod syscall_bindings;

mod bytestring;
mod crypto;
mod invoke;
mod nonnegative_int;

pub mod rt;

pub use bytestring::*;
pub use crypto::*;
pub use invoke::*;
pub use nonnegative_int::*;

pub use rs_xdr::*;
