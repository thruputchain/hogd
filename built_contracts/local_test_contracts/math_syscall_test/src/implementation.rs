use crate::MathSyscallTest;
use thruput_sdk::sys::SyscallErrorCode;
use thruput_sdk::*;
use thruput_xdr::syscalls::SyscallWord;

pub struct MathSyscallTestContract;

const NONE: i32 = SyscallErrorCode::kSyscallErrorCodeNone.0;
const FAULT: i32 = SyscallErrorCode::kSyscallErrorCodeFault.0;

/// U256Sqrt(value) -> root. The crates this fixture builds against predate
/// the syscall, so it is called by number.
const U256_SQRT: SyscallWord = 40;

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
}
