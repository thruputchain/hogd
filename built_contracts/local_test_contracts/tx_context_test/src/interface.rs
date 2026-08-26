use sdk::*;

/// Exercises the transaction-context syscalls that report facts about the
/// enclosing transaction and block: GetSourceTransactionHash (which has no
/// other contract-side coverage), GetBlockHeight, and the valid-input side of
/// GetTransactionStatistics (syscall_misc_tests covers only the invalid
/// statistic).
#[contract(client = Client)]
pub trait TxContextTest {
    /// Returns the source transaction hash as observed via the syscall, so
    /// the host-side test can compare it against the hash of the transaction
    /// it actually submitted.
    fn get_source_tx_hash() -> SHA256Hash;

    /// Returns the current block height as observed via the syscall.
    /// (BlockHeight is an i64 in the protocol XDR.)
    fn get_block_height() -> i64;

    /// GetTransactionStatistics(GasRemaining) succeeds and reports a nonzero
    /// remaining-gas value mid-execution.
    fn test_tx_stats_gas_remaining();

    /// Internal helper: reports the source tx hash from a child frame. Runs
    /// at a second deployment of this contract (the reentrance guard rejects
    /// a self-invoke).
    fn internal_get_source_tx_hash() -> SHA256Hash;

    /// The source tx hash is a per-transaction constant: a child frame at
    /// `target` must observe the same value as the root frame.
    fn test_source_tx_hash_in_child(target: &ContractAddress);
}
