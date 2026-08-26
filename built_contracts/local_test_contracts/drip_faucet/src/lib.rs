#![no_std]

#[cfg(feature = "dev")]
extern crate std;

mod implementation;
pub mod interface;

// A trait describing the contract interface
pub use interface::DripError;
pub use interface::DripFaucet;
pub use interface::DripFaucetEntrypoints;

// A client stub that provides the interface via cross-contract calls
pub use interface::Client;
