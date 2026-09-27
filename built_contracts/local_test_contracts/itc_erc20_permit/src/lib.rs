//! The RV32 reference for the native token's `ERC20Permit` surface.
//!
//! `itc_erc20` is the reference the native token's ERC20 methods are held
//! byte-identical to; this crate is that same contract -- the same storage
//! keys, the same strict allowance and balance updates, the same events --
//! with `ERC20Permit` implemented on top. Keeping the two in one contract
//! lets a conformance test spend a permit-written allowance through the
//! ordinary `transfer_from` and read it back through `allowance`, exactly as
//! it would against the native token.

#![no_std]

pub use groundhog_erc20_if as interface;
pub use groundhog_erc20_permit_if as permit_interface;

pub use interface::{ERC20, ERC20Client};
pub use permit_interface::{ERC20Permit, ERC20PermitClient};

mod implementation;

pub use implementation::Token;
