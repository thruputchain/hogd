//! A contract-side ERC165 caller: it asks another contract which interfaces
//! it implements the way real contracts do, through `ERC165Client`, so a
//! test can check what such a caller actually sees.

#![no_std]

mod implementation;
pub mod interface;

pub use interface::{Client, Erc165Probe};
