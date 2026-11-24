use crate::MathSyscallTest;
use thruput_sdk::sys::SyscallErrorCode;
use thruput_sdk::*;
use thruput_xdr::syscalls::SyscallWord;

pub struct MathSyscallTestContract;

const NONE: i32 = SyscallErrorCode::kSyscallErrorCodeNone.0;
const FAULT: i32 = SyscallErrorCode::kSyscallErrorCodeFault.0;
const INVALID_INPUT: i32 =
    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput.0;

/// U256Sqrt(value) -> root. The crates this fixture builds against predate
/// the math syscalls, so they are called by number.
const U256_SQRT: SyscallWord = 40;
/// U256MulDivRem(a, b, divisor) -> quotient_lo, quotient_hi, remainder.
const U256_MUL_DIV_REM: SyscallWord = 41;
/// U128MulDivRem, the same on 128-bit values.
const U128_MUL_DIV_REM: SyscallWord = 42;

/// One U256Sqrt with raw argument words: where the 32 bytes of the value are,
/// and where the 16 bytes of the root go.
fn u256_sqrt_at(value: SyscallWord, root: SyscallWord) -> i32 {
    thruput_syscall::syscall_wrapper(U256_SQRT, value, root, 0, 0, 0, 0, 0)
}

/// The root of a value given as its bytes, least significant first.
fn u256_sqrt(value: &[u8; 32]) -> u128 {
    let mut root = 0u128;
    let code = u256_sqrt_at(
        value.as_ptr() as SyscallWord,
        &mut root as *mut u128 as SyscallWord,
    );
    assert_eq!(code, NONE);
    root
}

/// A U256 as the syscall takes it.
fn to_le_bytes(value: U256) -> [u8; 32] {
    let mut bytes = [0u8; 32];
    for (chunk, limb) in bytes.chunks_exact_mut(4).zip(value.to_limbs()) {
        chunk.copy_from_slice(&limb.to_le_bytes());
    }
    bytes
}

/// And as the syscall returns it.
fn from_le_bytes(bytes: &[u8; 32]) -> U256 {
    let mut limbs = [0u32; 8];
    for (limb, chunk) in limbs.iter_mut().zip(bytes.chunks_exact(4)) {
        *limb = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
    }
    U256::from_limbs(limbs)
}

/// One MulDivRem of either width with raw argument words: where a, b and the
/// divisor are, then where quotient_lo, quotient_hi and the remainder go.
fn mul_div_rem_at(syscall: SyscallWord, words: [SyscallWord; 6]) -> i32 {
    thruput_syscall::syscall_wrapper(
        syscall, words[0], words[1], words[2], words[3], words[4], words[5], 0,
    )
}

/// U256MulDivRem of values given as their bytes: quotient_lo, quotient_hi
/// and the remainder.
fn u256_mul_div_rem(
    a: &[u8; 32],
    b: &[u8; 32],
    divisor: &[u8; 32],
) -> [[u8; 32]; 3] {
    let mut results = [[0u8; 32]; 3];
    let [quotient_lo, quotient_hi, remainder] = &mut results;
    let code = mul_div_rem_at(
        U256_MUL_DIV_REM,
        [
            a.as_ptr() as SyscallWord,
            b.as_ptr() as SyscallWord,
            divisor.as_ptr() as SyscallWord,
            quotient_lo.as_mut_ptr() as SyscallWord,
            quotient_hi.as_mut_ptr() as SyscallWord,
            remainder.as_mut_ptr() as SyscallWord,
        ],
    );
    assert_eq!(code, NONE);
    results
}

/// U128MulDivRem: quotient_lo, quotient_hi and the remainder.
fn u128_mul_div_rem(a: u128, b: u128, divisor: u128) -> [u128; 3] {
    let mut results = [0u128; 3];
    let [quotient_lo, quotient_hi, remainder] = &mut results;
    let code = mul_div_rem_at(
        U128_MUL_DIV_REM,
        [
            &a as *const u128 as SyscallWord,
            &b as *const u128 as SyscallWord,
            &divisor as *const u128 as SyscallWord,
            quotient_lo as *mut u128 as SyscallWord,
            quotient_hi as *mut u128 as SyscallWord,
            remainder as *mut u128 as SyscallWord,
        ],
    );
    assert_eq!(code, NONE);
    results
}

/// Checks U256MulDivRem of nonzero-divisor operands against the SDK's
/// software routines and against what the results are defined to be.
fn check_u256_mul_div_rem(a: U256, b: U256, divisor: U256) {
    let results = u256_mul_div_rem(
        &to_le_bytes(a),
        &to_le_bytes(b),
        &to_le_bytes(divisor),
    );
    let quotient_lo = from_le_bytes(&results[0]);
    let quotient_hi = from_le_bytes(&results[1]);
    let remainder = from_le_bytes(&results[2]);

    // The software mul_div has a quotient exactly when it fits 256 bits.
    match U256::checked_mul_div(a, b, divisor) {
        Some(quotient) => {
            assert!(quotient_hi.is_zero() && quotient_lo == quotient)
        }
        None => assert!(!quotient_hi.is_zero()),
    }
    assert!(U256::checked_mul_mod(a, b, divisor) == Some(remainder));

    // a * b == (quotient_hi * 2^256 + quotient_lo) * divisor + remainder,
    // as 512-bit values built with multiplication alone, and the remainder
    // is below the divisor.
    assert!(remainder < divisor);
    let (product_lo, product_hi) = a.widening_mul(b);
    let (low, low_carry) = quotient_lo.widening_mul(divisor);
    let (high, high_carry) = quotient_hi.widening_mul(divisor);
    assert!(high_carry.is_zero());
    let (rebuilt_lo, carried) = low.overflowing_add(remainder);
    let carry = if carried { U256::ONE } else { U256::ZERO };
    let rebuilt_hi = low_carry
        .checked_add(high)
        .and_then(|sum| sum.checked_add(carry));
    assert!(rebuilt_lo == product_lo && rebuilt_hi == Some(product_hi));
}

/// The same for U128MulDivRem.
fn check_u128_mul_div_rem(a: u128, b: u128, divisor: u128) {
    let [quotient_lo, quotient_hi, remainder] = u128_mul_div_rem(a, b, divisor);

    // The software mul_div has a quotient exactly when it fits 128 bits.
    match checked_mul_div_u128(a, b, divisor) {
        Some(quotient) => assert!(quotient_hi == 0 && quotient_lo == quotient),
        None => assert!(quotient_hi != 0),
    }

    // a * b == (quotient_hi * 2^128 + quotient_lo) * divisor + remainder,
    // and the remainder is below the divisor.
    assert!(remainder < divisor);
    let quotient = U256::from_u128(quotient_hi).wrapping_shl(128)
        + U256::from_u128(quotient_lo);
    let rebuilt = quotient
        .checked_mul(U256::from_u128(divisor))
        .and_then(|product| product.checked_add(U256::from_u128(remainder)));
    assert!(rebuilt == Some(U256::widening_mul_u128(a, b)));
}

/// The overlap rules of one MulDivRem call, with operands of `width` bytes.
/// Returns how many of its calls faulted, which is every one that should.
fn check_mul_div_rem_overlap(syscall: SyscallWord, width: usize) -> u32 {
    // Nine slots of 32 bytes. a, b and the divisor are at slots 1 to 3 and
    // the results go to slots 5 to 7 unless a call moves them; slots 0, 4
    // and 8 are spare room around them.
    const SLOT: usize = 32;
    const SLOTS: usize = 9;
    const UNTOUCHED: u8 = 0xa5;
    const INPUTS: [usize; 3] = [SLOT, 2 * SLOT, 3 * SLOT];
    const OUTPUTS: [usize; 3] = [5 * SLOT, 6 * SLOT, 7 * SLOT];
    type Arena = [u8; SLOTS * SLOT];

    // A small value at a slot: its low byte, then zeros.
    fn put(arena: &mut Arena, at: usize, width: usize, value: u8) {
        arena[at..at + width].fill(0);
        arena[at] = value;
    }

    // One call with each argument at a byte offset into the arena.
    let call = |arena: &mut Arena, inputs: [usize; 3], outputs: [usize; 3]| {
        let base = arena.as_mut_ptr() as usize;
        let word = |offset: usize| (base + offset) as SyscallWord;
        mul_div_rem_at(
            syscall,
            [
                word(inputs[0]),
                word(inputs[1]),
                word(inputs[2]),
                word(outputs[0]),
                word(outputs[1]),
                word(outputs[2]),
            ],
        )
    };

    // 6 * 7 / 4 is 10, remainder 2.
    let mut arena = [UNTOUCHED; SLOTS * SLOT];
    put(&mut arena, INPUTS[0], width, 6);
    put(&mut arena, INPUTS[1], width, 7);
    put(&mut arena, INPUTS[2], width, 4);
    let before = arena;
    let mut faults = 0;

    // A call that succeeds writes these three results at `outputs` and
    // nothing else.
    let expect_results =
        |inputs: [usize; 3], outputs: [usize; 3], results: [u8; 3]| {
            let mut arena = before;
            assert_eq!(call(&mut arena, inputs, outputs), NONE);
            let mut expected = before;
            for (at, value) in outputs.into_iter().zip(results) {
                put(&mut expected, at, width, value);
            }
            assert!(arena == expected);
        };

    // Apart, and back to back: the first result ends where the second
    // begins, and the divisor ends where the first result begins.
    expect_results(INPUTS, OUTPUTS, [10, 0, 2]);
    let touching = [
        INPUTS[2] + width,
        INPUTS[2] + 2 * width,
        INPUTS[2] + 3 * width,
    ];
    expect_results(INPUTS, touching, [10, 0, 2]);

    // Inputs may share a buffer: one for both factors, 6 * 6 / 4, and one
    // for all three, 6 * 6 / 6.
    expect_results([INPUTS[0], INPUTS[0], INPUTS[2]], OUTPUTS, [9, 0, 0]);
    expect_results([INPUTS[0]; 3], OUTPUTS, [6, 0, 0]);

    // Inputs may also overlap in part. b one byte into a reads a's zeros
    // and then one byte beyond them, so it is zero or a power of 256 times
    // that byte; either way the call succeeds, and writes what it writes
    // when given a copy of that b on its own.
    {
        let overlapping = [INPUTS[0], INPUTS[0] + 1, INPUTS[2]];
        let mut arena = before;
        assert_eq!(call(&mut arena, overlapping, OUTPUTS), NONE);
        let mut apart = before;
        apart.copy_within(INPUTS[0] + 1..INPUTS[0] + 1 + width, 0);
        assert_eq!(call(&mut apart, [INPUTS[0], 0, INPUTS[2]], OUTPUTS), NONE);
        for at in OUTPUTS {
            assert!(arena[at..at + width] == apart[at..at + width]);
        }
    }

    // A call that faults writes nothing.
    let mut expect_fault = |inputs: [usize; 3], outputs: [usize; 3]| {
        let mut arena = before;
        assert_eq!(call(&mut arena, inputs, outputs), FAULT);
        assert!(arena == before);
        faults += 1;
    };

    // An output on an input: all of it, or one byte at either end.
    for moved in 0..3 {
        for input in INPUTS {
            for at in [input, input + width - 1, input + 1 - width] {
                let mut outputs = OUTPUTS;
                outputs[moved] = at;
                expect_fault(INPUTS, outputs);
            }
        }
    }

    // An output on another output, the same three ways.
    for (moved, onto) in [(1, 0), (2, 0), (2, 1)] {
        let other = OUTPUTS[onto];
        for at in [other, other + width - 1, other + 1 - width] {
            let mut outputs = OUTPUTS;
            outputs[moved] = at;
            expect_fault(INPUTS, outputs);
        }
    }

    faults
}

/// Checks the syscall's root of `value` against the software square root and
/// against what a floor square root is.
fn check_root(value: U256) {
    let root = u256_sqrt(&to_le_bytes(value));
    assert!(value.integer_sqrt() == U256::from_u128(root));

    // root * root <= value < (root + 1) * (root + 1), the second half as
    // value - root * root <= 2 * root so that nothing overflows.
    let square = U256::widening_mul_u128(root, root);
    assert!(square <= value);
    let twice_root = U256::from_u128(root) + U256::from_u128(root);
    assert!(value - square <= twice_root);
}

/// A fixed stream of 64-bit words (splitmix64), so that every run checks the
/// same values.
struct Stream(u64);

impl Stream {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    fn next_u128(&mut self) -> u128 {
        ((self.next() as u128) << 64) | self.next() as u128
    }

    fn next_u256(&mut self) -> U256 {
        let mut limbs = [0u32; 8];
        for pair in limbs.chunks_exact_mut(2) {
            let word = self.next();
            pair[0] = word as u32;
            pair[1] = (word >> 32) as u32;
        }
        U256::from_limbs(limbs)
    }
}

#[contract]
impl MathSyscallTest for MathSyscallTestContract {
    fn u256_sqrt(value: [u8; 32]) -> [u8; 16] {
        u256_sqrt(&value).to_le_bytes()
    }

    fn test_u256_sqrt_matches_software() {
        let mut stream = Stream(0x5eed);
        let top_bit = U256::ONE.wrapping_shl(255);

        check_root(U256::ZERO);
        check_root(U256::MAX);

        // A value of each length from 1 bit to 256, in steps that visit
        // every position within a 32-bit limb.
        for shift in (0..256).step_by(3) {
            let value = (stream.next_u256() | top_bit).wrapping_shr(shift);
            check_root(value);
        }

        // A root of each length from 1 bit to 128: its square, the value
        // just below it, and the largest value with the same root.
        for shift in 0..128 {
            let root = (stream.next_u128() | (1 << 127)) >> shift;
            let square = U256::widening_mul_u128(root, root);
            let twice_root = U256::from_u128(root) + U256::from_u128(root);
            check_root(square);
            check_root(square - U256::ONE);
            check_root(square + twice_root);
        }
    }

    fn test_u256_sqrt_root_may_not_overlap_value() {
        // The value at 0x10..0x30, with room for a root on either side.
        const VALUE: usize = 0x10;
        const UNTOUCHED: u8 = 0xa5;
        let mut arena = [UNTOUCHED; 0x40];
        arena[VALUE..VALUE + 32].fill(0);
        arena[VALUE] = 4;
        let before = arena;

        // One call with the value at VALUE and the root at `root`.
        fn sqrt_into(arena: &mut [u8; 0x40], root: usize) -> i32 {
            let base = arena.as_mut_ptr() as usize;
            u256_sqrt_at(
                (base + VALUE) as SyscallWord,
                (base + root) as SyscallWord,
            )
        }

        // On the value's low half, on its high half, and on one byte at
        // either end of it.
        for root in [VALUE, VALUE + 16, VALUE - 15, VALUE + 31] {
            assert_eq!(sqrt_into(&mut arena, root), FAULT);
            assert!(arena == before);
        }

        // Back to back with the value, below it and above it.
        for root in [VALUE - 16, VALUE + 32] {
            assert_eq!(sqrt_into(&mut arena, root), NONE);
            let mut expected = before;
            expected[root..root + 16].fill(0);
            expected[root] = 2;
            assert!(arena == expected);
            arena = before;
        }
    }

    fn u256_mul_div_rem(
        a: [u8; 32],
        b: [u8; 32],
        divisor: [u8; 32],
    ) -> [u8; 96] {
        let results = u256_mul_div_rem(&a, &b, &divisor);
        let mut bytes = [0u8; 96];
        for (chunk, result) in bytes.chunks_exact_mut(32).zip(results) {
            chunk.copy_from_slice(&result);
        }
        bytes
    }

    fn u128_mul_div_rem(
        a: [u8; 16],
        b: [u8; 16],
        divisor: [u8; 16],
    ) -> [u8; 48] {
        let results = u128_mul_div_rem(
            u128::from_le_bytes(a),
            u128::from_le_bytes(b),
            u128::from_le_bytes(divisor),
        );
        let mut bytes = [0u8; 48];
        for (chunk, result) in bytes.chunks_exact_mut(16).zip(results) {
            chunk.copy_from_slice(&result.to_le_bytes());
        }
        bytes
    }

    fn test_u256_mul_div_rem_matches_software() {
        let mut stream = Stream(0x5eed_0256);
        let top_bit = U256::ONE.wrapping_shl(255);

        // The extremes: the largest quotient, the largest that fits, and
        // the smallest operands.
        check_u256_mul_div_rem(U256::MAX, U256::MAX, U256::ONE);
        check_u256_mul_div_rem(U256::MAX, U256::MAX, U256::MAX);
        check_u256_mul_div_rem(U256::MAX, U256::MAX, top_bit);
        check_u256_mul_div_rem(U256::ZERO, U256::MAX, U256::MAX);
        check_u256_mul_div_rem(U256::ONE, U256::ONE, U256::ONE);

        // Each operand in turn takes each length from 1 bit to 256, in
        // steps that visit every position within a 32-bit limb, while the
        // other two take lengths at random.
        for shift in (0..256).step_by(5) {
            for position in 0..3 {
                let mut shifts = [0u32; 3];
                for other in shifts.iter_mut() {
                    *other = (stream.next() % 256) as u32;
                }
                shifts[position] = shift;
                let [a, b, divisor] = shifts.map(|shift| {
                    (stream.next_u256() | top_bit).wrapping_shr(shift)
                });
                check_u256_mul_div_rem(a, b, divisor);
            }
        }
    }

    fn test_u128_mul_div_rem_matches_software() {
        let mut stream = Stream(0x5eed_0128);
        let top_bit = 1u128 << 127;

        check_u128_mul_div_rem(u128::MAX, u128::MAX, 1);
        check_u128_mul_div_rem(u128::MAX, u128::MAX, u128::MAX);
        check_u128_mul_div_rem(u128::MAX, u128::MAX, top_bit);
        check_u128_mul_div_rem(0, u128::MAX, u128::MAX);
        check_u128_mul_div_rem(1, 1, 1);

        // Each operand in turn takes each length from 1 bit to 128, the
        // other two lengths at random.
        for shift in (0..128).step_by(3) {
            for position in 0..3 {
                let mut shifts = [0u32; 3];
                for other in shifts.iter_mut() {
                    *other = (stream.next() % 128) as u32;
                }
                shifts[position] = shift;
                let [a, b, divisor] =
                    shifts.map(|shift| (stream.next_u128() | top_bit) >> shift);
                check_u128_mul_div_rem(a, b, divisor);
            }
        }
    }

    fn test_mul_div_rem_zero_divisor() {
        const UNTOUCHED: u8 = 0xa5;

        // 7 * 9 / 0, and 0 * 0 / 0, at 256 bits.
        let zero = [0u8; 32];
        let mut seven = zero;
        seven[0] = 7;
        let mut nine = zero;
        nine[0] = 9;
        for (a, b) in [(&seven, &nine), (&zero, &zero)] {
            let mut results = [[UNTOUCHED; 32]; 3];
            let [quotient_lo, quotient_hi, remainder] = &mut results;
            let code = mul_div_rem_at(
                U256_MUL_DIV_REM,
                [
                    a.as_ptr() as SyscallWord,
                    b.as_ptr() as SyscallWord,
                    zero.as_ptr() as SyscallWord,
                    quotient_lo.as_mut_ptr() as SyscallWord,
                    quotient_hi.as_mut_ptr() as SyscallWord,
                    remainder.as_mut_ptr() as SyscallWord,
                ],
            );
            assert_eq!(code, INVALID_INPUT);
            assert!(results == [[UNTOUCHED; 32]; 3]);
        }

        // The same at 128 bits.
        const UNTOUCHED_U128: u128 = u128::from_le_bytes([UNTOUCHED; 16]);
        let zero = 0u128;
        for (a, b) in [(7u128, 9u128), (0, 0)] {
            let mut results = [UNTOUCHED_U128; 3];
            let [quotient_lo, quotient_hi, remainder] = &mut results;
            let code = mul_div_rem_at(
                U128_MUL_DIV_REM,
                [
                    &a as *const u128 as SyscallWord,
                    &b as *const u128 as SyscallWord,
                    &zero as *const u128 as SyscallWord,
                    quotient_lo as *mut u128 as SyscallWord,
                    quotient_hi as *mut u128 as SyscallWord,
                    remainder as *mut u128 as SyscallWord,
                ],
            );
            assert_eq!(code, INVALID_INPUT);
            assert!(results == [UNTOUCHED_U128; 3]);
        }
    }

    fn test_mul_div_rem_overlap() {
        // 27 placements of an output on an input and 9 of an output on
        // another output, for each call. The host-side test expects 72.
        assert_eq!(check_mul_div_rem_overlap(U256_MUL_DIV_REM, 32), 36);
        assert_eq!(check_mul_div_rem_overlap(U128_MUL_DIV_REM, 16), 36);
    }
}
