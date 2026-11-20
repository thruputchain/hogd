use crate::{Client, Erc165Probe};
use thruput_erc20_if::ERC20Client;
use thruput_erc20_permit_if::ERC20PermitClient;
use thruput_erc165_if::{ERC165Client, InterfaceId};
use thruput_sdk::*;

pub struct Erc165ProbeContract;

#[contract]
impl Erc165Probe for Erc165ProbeContract {
    fn supports(target: &ContractAddress) -> (bool, bool, bool, bool) {
        let [erc20, permit, erc165, probe] =
            <ERC165Client as ContractClient>::new(target).supports::<(
                ERC20Client,
                ERC20PermitClient,
                ERC165Client,
                Client,
            )>();
        (erc20, permit, erc165, probe)
    }

    fn probe_interface(
        target: &ContractAddress,
        interface_id: InterfaceId,
    ) -> Option<bool> {
        <ERC165Client as ContractClient>::new(target)
            .with_read_only()
            .try_supports_interface(interface_id)
            .ok()
    }
}
