use crate::FaucetCaller;
use sdk::*;

pub struct FaucetCallerContract;

#[contract]
impl FaucetCaller for FaucetCallerContract {
    fn claim_from(faucet: &ContractAddress, amount: i128) {
        airdrop_faucet::Client::new(faucet).claim(amount);
    }
}
