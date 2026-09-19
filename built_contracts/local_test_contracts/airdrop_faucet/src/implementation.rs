use crate::AirdropFaucet;
use groundhog_erc20_if::ERC20Client;
use sdk::*;

/// Funds the token contract's own rows (its balance row and the total-supply
/// row): the faucet-root analog of the stock genesis ceremony's
/// `token_funding` mint.
const TOKEN_ROW_FUNDING: u128 = 1_000_000;
/// Funds this contract's own rows (the bootstrap marker below): the analog of
/// the stock ceremony's `root_funding` mint. A real chain sizes both of these
/// to its storage cost model; these are comfortably above the default model's
/// costs.
const ROOT_ROW_FUNDING: u128 = 1_000_000;

/// Marks the chain as bootstrapped.
///
/// Written with an *exclusive* set, which is what makes the bootstrap
/// one-time: two claims that both take this path cannot be included in the
/// same block, so exactly one of them funds the rows and the other is
/// rejected and can retry. A guard on the token's balance or total supply
/// would not serialize -- those are commutative, so concurrent first claims
/// would each read zero and each mint the funding.
///
/// Ordinary (post-bootstrap) claims only read this key, so they never
/// contend with each other.
fn bootstrap_key() -> StorageKey {
    b"airdrop_bootstrapped".hash().into()
}

pub struct AirdropFaucetContract;

#[contract]
impl AirdropFaucet for AirdropFaucetContract {
    fn claim(amount: u128) {
        let receiver = get_caller();
        let token_address = sdk::precompiles::ADDRESS_PRECOMPILED_HOG_TOKEN;
        let token = ERC20Client::new(&token_address);

        let bootstrapped = non_negative_int_try_get(&bootstrap_key())
            .none_if_missing()
            .unwrap_or(0);
        if bootstrapped == 0 {
            non_negative_int_set_exclusive(&bootstrap_key(), 1);
            token.mint(&token_address, TOKEN_ROW_FUNDING);
            token.mint(&get_current_address(), ROOT_ROW_FUNDING);
        }

        token.mint(&receiver, amount);
    }
}
