use crate::AirdropFaucet;
use groundhog_erc20_if::ERC20Client;
use sdk::*;

/// Minted to the token contract itself by the chain's first claim, covering
/// the storage costs of the token's own rows (its balance row and the
/// total-supply row) -- the faucet-root analog of the stock genesis
/// ceremony's `token_funding` mint. A real chain sizes this to its storage
/// cost model; this is comfortably above the default model's costs.
const TOKEN_ROW_FUNDING: u128 = 1_000_000;

pub struct AirdropFaucetContract;

#[contract]
impl AirdropFaucet for AirdropFaucetContract {
    fn claim(amount: u128) {
        let receiver = get_caller();
        let token_address = sdk::precompiles::ADDRESS_PRECOMPILED_HOG_TOKEN;
        let token = ERC20Client::new(&token_address);
        // The row funding is consumed only partially, so a positive balance
        // means the chain is already bootstrapped and the mint is skipped.
        if token.get_balance_of(&token_address) == 0 {
            token.mint(&token_address, TOKEN_ROW_FUNDING);
        }
        token.mint(&receiver, amount);
    }
}
