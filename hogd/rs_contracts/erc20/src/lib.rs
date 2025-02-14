//! ERC20 contract interface and implementation
//!
//! This version uses Result<T> as method return values

#![no_std]

#[cfg(feature = "std")]
extern crate std;

#[cfg(all(feature = "alloc", not(feature = "std")))]
extern crate alloc;

//#[cfg(all(feature = "client", feature = "guest"))]
//compile_error!("Cannot enable both features \"client\" and \"guest\"");

// A trait describing the contract interface
pub mod interface;

// A client stub that provides the interface via cross-contract calls
#[cfg(all(feature = "client", not(feature = "result")))]
pub use interface::client;
#[cfg(all(feature = "client", feature = "result"))]
pub use interface::client_result;

#[cfg(feature = "guest")]
// An implementation of the contract interface
mod guest;

// Contract methods exposed with C ABI for running as guest
#[cfg(feature = "guest")]
pub use guest::exports::*;

#[cfg(test)]
mod tests;
