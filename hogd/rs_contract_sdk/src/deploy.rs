#![allow(non_snake_case)]

use crate::syscall_bindings::{uninit_DeployContract, uninit_GetDeployAddress};
use core::mem::MaybeUninit;
use rs_xdr::{ContractAddress, SHA256Hash};

pub fn GetDeployAddress(
    nonce: &SHA256Hash,
    contract_hash: &SHA256Hash,
) -> Result<ContractAddress, i32> {
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();

    uninit_GetDeployAddress(None, nonce, contract_hash, &mut out_addr)?;

    Ok(unsafe { out_addr.assume_init() })
}

pub fn GetDeployAddressForOther(
    other_address: &ContractAddress,
    nonce: &SHA256Hash,
    contract_hash: &SHA256Hash,
) -> Result<ContractAddress, i32> {
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();

    uninit_GetDeployAddress(
        Some(other_address),
        nonce,
        contract_hash,
        &mut out_addr,
    )?;

    Ok(unsafe { out_addr.assume_init() })
}

pub fn DeployContract(
    nonce: &SHA256Hash,
    contract_hash: &SHA256Hash,
) -> Result<ContractAddress, i32> {
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();
    uninit_DeployContract(nonce, contract_hash, &mut out_addr)?;

    Ok(unsafe { out_addr.assume_init() })
}
