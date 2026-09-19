use sdk::*;

/// A contract-shaped faucet user for the bootstrap-loop scenario: a pubkey
/// account funds itself from the airdrop faucet, deploys this contract
/// through the default wallet, and then this contract claims for itself --
/// the faucet's msg_sender is this contract's address, so the mint lands
/// here.
#[contract(client = Client)]
pub trait FaucetCaller {
    /// Claim `amount` from the airdrop faucet at `faucet` (normally the
    /// genesis-root address). The minted tokens credit this contract.
    fn claim_from(faucet: &ContractAddress, amount: u128);
}
