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

    /// U256MulDivRem of `a`, `b` and `divisor`: quotient_lo, then quotient_hi,
    /// then the remainder, 32 bytes each. The host-side test chooses the
    /// operands and checks the results.
    fn u256_mul_div_rem(
        a: [u8; 32],
        b: [u8; 32],
        divisor: [u8; 32],
    ) -> [u8; 96];

    /// U128MulDivRem, the same at half the width: 16 bytes each.
    fn u128_mul_div_rem(
        a: [u8; 16],
        b: [u8; 16],
        divisor: [u8; 16],
    ) -> [u8; 48];

    /// U256MulDivRem agrees with the SDK's software mul_div and mul_mod, and
    /// its results satisfy a * b = quotient * divisor + remainder with the
    /// remainder below the divisor, over operands of every size.
    fn test_u256_mul_div_rem_matches_software();

    /// The same for U128MulDivRem and the SDK's mul_div_u128.
    fn test_u128_mul_div_rem_matches_software();

    /// A zero divisor is an invalid input to either call, and leaves the
    /// outputs as they were.
    fn test_mul_div_rem_zero_divisor();

    /// For either call, an output may overlap nothing, input or output,
    /// while the inputs may share a buffer.
    fn test_mul_div_rem_overlap();
}
