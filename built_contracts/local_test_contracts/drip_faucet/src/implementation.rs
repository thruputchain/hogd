use crate::interface::DripError;
use crate::DripFaucet;
use groundhog_erc20_if::ERC20Client;
use sdk::storage::{CapacityConstraint, Keypath};
use sdk::*;

/// Blocks per budget window: a reset is allowed once this many blocks have
/// passed since the last one.
const PERIOD_BLOCKS: i64 = 5;
/// Native tokens mintable per window, across all callers.
const PERIOD_LIMIT: i128 = 100;

/// Height of the last window reset, or missing before the first claim.
fn last_reset_key() -> StorageKey {
    b"drip_last_reset".hash().into()
}

/// Remaining window budget. Consumption is a commutative capacity decrement
/// (validated in aggregate at block packing); only the window reset writes an
/// explicit value, and same-value resets from concurrent transactions
/// compose.
fn window_capacity() -> CapacityConstraint {
    Keypath::default().into()
}

fn current_block_height() -> i64 {
    let mut height: BlockHeight = 0.into();
    sys::raw_GetBlockHeight(&mut height).unwrap();
    height.into_inner()
}

pub struct DripFaucetContract;

#[contract]
impl DripFaucet for DripFaucetContract {
    fn claim(amount: u128) {
        let mut capacity = window_capacity();

        let height = current_block_height();
        let last_reset = non_negative_int_try_get(&last_reset_key())
            .none_if_missing()
            .unwrap_or(-1);
        if last_reset < 0 || height - (last_reset as i64) >= PERIOD_BLOCKS {
            capacity.set_capacity(PERIOD_LIMIT);
            non_negative_int_set(&last_reset_key(), height as i128);
        }

        // Reject an overdraw this transaction can see (committed base plus
        // in-transaction consumption); an overdraw only visible across
        // concurrent transactions is left to the block-packing bound.
        let remaining = capacity.get_remaining_capacity();
        let Ok(signed_amount) = i128::try_from(amount) else {
            throw(DripError::ExceedsWindowBudget);
        };
        if remaining < signed_amount {
            throw(DripError::ExceedsWindowBudget);
        }
        capacity.consume_capacity(signed_amount);

        let receiver = get_caller();
        let token =
            ERC20Client::new(&sdk::precompiles::ADDRESS_PRECOMPILED_HOG_TOKEN);
        token.mint(&receiver, amount);
    }
}
