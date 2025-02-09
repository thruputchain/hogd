#![allow(non_snake_case)]

use crate::result::SysResult as Result;
use crate::sys::{uninit_DeployContract, uninit_GetDeployAddress};
use crate::types::{ContractAddress, SHA256Hash};
use core::mem::MaybeUninit;

pub fn GetDeployAddress(
    nonce: &SHA256Hash,
    contract_hash: &SHA256Hash,
    ) -> Result<ContractAddress>
{
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();

    uninit_GetDeployAddress(None, nonce, contract_hash, &mut out_addr)?;

    Ok(unsafe { out_addr.assume_init() })
}

pub fn GetDeployAddressForOther(
    other_address: &ContractAddress,
    nonce: &SHA256Hash,
    contract_hash: &SHA256Hash,
    ) -> Result<ContractAddress>
{
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
) -> Result<ContractAddress>
{
    let mut out_addr: MaybeUninit<ContractAddress> =
        MaybeUninit::<ContractAddress>::uninit();
    uninit_DeployContract(nonce, contract_hash, &mut out_addr)?;

    Ok(unsafe { out_addr.assume_init() })
}
