use sdk::*;

/// A testnet airdrop faucet meant to be installed as the genesis ROOT
/// contract: the native token's mint is gated on
/// msg_sender == the genesis-root address, so `claim` only works from that
/// deployment. Anyone who asks gets the amount they ask for.
#[contract(client = Client)]
pub trait AirdropFaucet {
    /// Mint `amount` native tokens to the caller.
    fn claim(amount: u128);
}
