//! ERC20 contract interface and implementation
//!
//! This version uses Result<T> as method return values

#![no_std]

#[cfg(feature = "std")]
extern crate std;

#[cfg(all(feature = "alloc", not(feature = "std")))]
extern crate alloc;

// A trait describing the contract interface
pub mod interface;

#[cfg(feature = "client")]
// A client stub that provides the interface via cross-contract calls
pub use interface::client;

#[cfg(feature = "guest")]
// An implementation of the contract interface
mod guest;

#[cfg(feature = "guest")]
// Contract methods exposed with C ABI for running as guest
pub use guest::exports::*;
