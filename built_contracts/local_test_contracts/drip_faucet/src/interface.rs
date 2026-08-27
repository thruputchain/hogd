use sdk::*;

#[contract_exception]
#[derive(Copy)]
pub enum DripError {
    /// The requested amount exceeds what is left of the current window's
    /// budget (as visible within this transaction).
    ExceedsWindowBudget = 1,
}

/// A rate-limited faucet meant to be installed as the genesis ROOT contract:
/// mints at most `kPeriodLimit` (100) native tokens per `kPeriodBlocks` (5)
/// block window, globally, first come first served.
///
/// The window budget is a `CapacityConstraint`, so claims in the same window
/// stage commutative capacity decrements: concurrent claim transactions in
/// one block compose, and only an aggregate overdraw is rejected (at block
/// packing, as a bounds conflict). A single transaction that overdraws the
/// budget it can see is rejected here with `ExceedsWindowBudget` instead.
#[contract(client = Client, throws = DripError)]
pub trait DripFaucet {
    /// Mint `amount` native tokens to the caller, consuming window budget.
    fn claim(amount: u128);
}
