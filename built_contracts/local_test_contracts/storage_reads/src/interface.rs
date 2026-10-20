use thruput_sdk::*;

/// A transaction whose cost is dominated by database round trips rather than
/// compute: each key read suspends the executor on a HogDb read. Driven by
/// `hogd/hog_exec/bench/makespan_runner.cc`.
#[contract(client = Client)]
pub trait StorageReads {
    /// No-op entrypoint for the genesis deployment, which must invoke
    /// something.
    fn init();

    /// Reads `count` keys derived from `seed`, one after another, and returns
    /// how many were present. The bench passes a fresh `seed` per transaction,
    /// so none of the reads are answered from the executor's per-block cache.
    fn read_keys(seed: u64, count: u32) -> u32;
}
