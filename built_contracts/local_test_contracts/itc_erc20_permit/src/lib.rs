//! `itc_erc20` with `ERC20Permit` on top: the same storage keys, updates and
//! events. The RV32 reference for the native token's permit surface.

#![no_std]

pub use groundhog_erc20_if as interface;
pub use groundhog_erc20_permit_if as permit_interface;

pub use interface::{ERC20, ERC20Client};
pub use permit_interface::{ERC20Permit, ERC20PermitClient};

mod implementation;

pub use implementation::Token;
