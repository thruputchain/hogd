//! `itc_erc20` with `ERC20Permit` and `ERC165` on top: the same storage keys,
//! updates and events. The RV32 reference for the native token's permit and
//! ERC-165 surfaces.

#![no_std]

pub use thruput_erc20_if as interface;
pub use thruput_erc20_permit_if as permit_interface;

pub use interface::{ERC20, ERC20Client};
pub use permit_interface::{ERC20Permit, ERC20PermitClient};
pub use thruput_erc165_if::{ERC165, ERC165Client};

mod implementation;

pub use implementation::Token;
