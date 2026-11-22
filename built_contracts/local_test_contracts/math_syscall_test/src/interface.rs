use thruput_sdk::*;

/// The math syscalls, as a guest sees them.
///
/// A 256-bit integer crosses the syscall boundary as 32 bytes, least
/// significant first, and so does every one in this interface.
#[contract(client = Client)]
pub trait MathSyscallTest {
    /// U256Sqrt of `value`: the 16 bytes of the root, least significant
    /// first. The host-side test chooses the value and checks the root.
    fn u256_sqrt(value: [u8; 32]) -> [u8; 16];

    /// U256Sqrt agrees with the SDK's software square root, and its result
    /// is the largest root whose square does not exceed the value, over
    /// values of every size and the perfect squares and their neighbours.
    fn test_u256_sqrt_matches_software();

    /// U256Sqrt cannot write the root over the value it reads: an output may
    /// overlap nothing. A root that only touches the value is fine.
    fn test_u256_sqrt_root_may_not_overlap_value();
}
