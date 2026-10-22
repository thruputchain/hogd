use thruput_erc165_if::InterfaceId;
use thruput_sdk::*;

#[contract(client = Client)]
pub trait Erc165Probe {
    /// `ERC165Client::supports` against `target` for ERC20, ERC20Permit,
    /// ERC165 and this probe's own interface, in that order: one read-only
    /// `supports_interfaces` call, in which any failure reads as unsupported.
    fn supports(target: &ContractAddress) -> (bool, bool, bool, bool);

    /// One read-only `supports_interface` call on `target`, or `None` when
    /// the call itself failed, which `supports` cannot tell from `false`.
    fn probe_interface(
        target: &ContractAddress,
        interface_id: InterfaceId,
    ) -> Option<bool>;
}
