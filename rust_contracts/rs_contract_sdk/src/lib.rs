#![cfg_attr(not(feature = "std"), no_std)]
#![allow(unused_imports)]

mod rt;
pub use rt::*;

#[cfg(feature = "std")]
mod std_error;
#[cfg(feature = "std")]
pub use std_error::*;

#[cfg(feature = "std")]
mod mock_syscall;
#[cfg(feature = "std")]
pub use mock_syscall::*;
