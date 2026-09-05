#![no_std]

#[cfg(feature = "dev")]
extern crate std;

mod implementation;
pub mod interface;

// A trait describing the contract interface
pub use interface::FaucetCaller;
pub use interface::FaucetCallerEntrypoints;

// A client stub that provides the interface via cross-contract calls
pub use interface::Client;
