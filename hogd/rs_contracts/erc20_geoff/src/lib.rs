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

// A client stub that provides the interface via cross-contract calls
#[cfg(feature = "client")]
pub use interface::client;

// An implementation of the contract interface
#[cfg(feature = "guest")]
mod guest;

// Contract methods exposed with C ABI for running as guest
#[cfg(feature = "guest")]
pub use guest::exports::*;

#[cfg(test)]
mod tests;
