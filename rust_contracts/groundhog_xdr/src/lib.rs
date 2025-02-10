#![cfg_attr(not(feature = "std"), no_std)]
#![allow(non_snake_case)]

#[rustfmt::skip]
pub mod contract;
#[rustfmt::skip]
pub mod syscall;
#[rustfmt::skip]
pub mod transaction;
#[rustfmt::skip]
pub mod types;
#[rustfmt::skip]
pub mod ffi_enums;

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

use contract::*;
use ffi_enums::*;
use syscall::*;
use transaction::*;
use types::*;

#[cfg(test)]
mod test;
