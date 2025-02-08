#![cfg_attr(not(feature = "std"), no_std)]

#[rustfmt::skip]
mod contract;
#[rustfmt::skip]
mod syscall;
#[rustfmt::skip]
mod transaction;
#[rustfmt::skip]
mod types;
#[rustfmt::skip]
mod ffi_enums;

mod common_types;
pub use common_types::*;

#[cfg(feature = "std")]
mod serialize;
#[cfg(feature = "std")]
pub use serialize::*;

#[cfg(feature = "std")]
mod syscall_args;
#[cfg(feature = "std")]
pub use syscall_args::*;

pub use contract::*;
pub use ffi_enums::*;
pub use syscall::*;
pub use transaction::*;
pub use types::*;

#[cfg(test)]
mod test;
