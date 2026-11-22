use crate::InputAliasTest;
use thruput_sdk::sys::{SyscallErrorCode, SyscallNo};
use thruput_sdk::*;
use thruput_xdr::syscalls::SyscallWord;

pub struct InputAliasTestContract;

const NONE: i32 = SyscallErrorCode::kSyscallErrorCodeNone.0;
const FAULT: i32 = SyscallErrorCode::kSyscallErrorCodeFault.0;

/// The argument word that leaves an optional pointer input out.
const ABSENT: SyscallWord = SyscallWord::MAX;

/// One syscall with raw argument words: unused words are zero.
fn syscall<const N: usize>(number: SyscallNo, args: [SyscallWord; N]) -> i32 {
    let mut words = [0 as SyscallWord; 7];
    words[..N].copy_from_slice(&args);
    thruput_syscall::syscall_wrapper(
        number.0 as SyscallWord,
        words[0],
        words[1],
        words[2],
        words[3],
        words[4],
        words[5],
        words[6],
    )
}

// Offsets into an arena, far enough apart that nothing overlaps unless a
// method makes it.
const A: usize = 0x40;
const B: usize = 0x80;
const C: usize = 0xc0;
const OUT: usize = 0x100;
const ARENA_LEN: usize = 0x180;

/// What every arena byte holds until something is written there.
const UNTOUCHED: u8 = 0xa5;

/// The memory a method's syscall arguments point into. Every argument is an
/// offset into the one array, so a method decides exactly which bytes two
/// arguments share.
struct Arena {
    bytes: [u8; ARENA_LEN],
}

impl Arena {
    fn new() -> Self {
        Arena {
            bytes: [UNTOUCHED; ARENA_LEN],
        }
    }

    /// The argument word for the byte at `offset`.
    fn at(&mut self, offset: usize) -> SyscallWord {
        (self.bytes.as_mut_ptr() as usize + offset) as SyscallWord
    }

    /// Thirty-two distinct bytes at `offset`, counting up from `first`.
    fn count_from(&mut self, offset: usize, first: u8) {
        for (i, byte) in self.bytes[offset..offset + 32].iter_mut().enumerate()
        {
            *byte = first + i as u8;
        }
    }

    fn get32(&self, offset: usize) -> [u8; 32] {
        let mut out = [0u8; 32];
        out.copy_from_slice(&self.bytes[offset..offset + 32]);
        out
    }

    fn u32_at(&self, offset: usize) -> u32 {
        let mut out = [0u8; 4];
        out.copy_from_slice(&self.bytes[offset..offset + 4]);
        u32::from_le_bytes(out)
    }

    fn untouched(&self, offset: usize, len: usize) -> bool {
        self.bytes[offset..offset + len]
            .iter()
            .all(|b| *b == UNTOUCHED)
    }
}

/// GetDeployAddress(caller_address?, nonce, contract_hash) -> to_be_deployed
/// hashes its inputs, so its output says which bytes each input was read as.
fn get_deploy_address(
    arena: &mut Arena,
    caller: Option<usize>,
    nonce: usize,
    contract_hash: usize,
    out: usize,
) -> i32 {
    let caller = match caller {
        Some(offset) => arena.at(offset),
        None => ABSENT,
    };
    syscall(
        SyscallNo::kGetDeployAddress,
        [
            caller,
            arena.at(nonce),
            arena.at(contract_hash),
            arena.at(out),
        ],
    )
}

/// BytestringSet(key, bytestring).
fn bytestring_set(
    arena: &mut Arena,
    key: usize,
    value: usize,
    len: usize,
) -> i32 {
    syscall(
        SyscallNo::kBytestringSet,
        [arena.at(key), arena.at(value), len as SyscallWord],
    )
}

/// BytestringGet(key) -> bytestring, written_len.
fn bytestring_get(
    arena: &mut Arena,
    key: usize,
    buffer: usize,
    len: usize,
    written: usize,
) -> i32 {
    syscall(
        SyscallNo::kBytestringGet,
        [
            arena.at(key),
            arena.at(buffer),
            len as SyscallWord,
            arena.at(written),
        ],
    )
}

/// The address GetDeployAddress gives when every input has thirty-two bytes
/// of its own: the reference a shared layout is compared against.
fn reference_address(
    caller_first: Option<u8>,
    nonce_first: u8,
    hash_first: u8,
) -> [u8; 32] {
    let mut arena = Arena::new();
    let caller = caller_first.map(|first| {
        arena.count_from(A, first);
        A
    });
    arena.count_from(B, nonce_first);
    arena.count_from(C, hash_first);
    assert_eq!(get_deploy_address(&mut arena, caller, B, C, OUT), NONE);
    let address = arena.get32(OUT);
    assert!(address != [UNTOUCHED; 32]);
    address
}

/// SHA-256 of no bytes.
const EMPTY_SHA256: [u8; 32] = [
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8,
    0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
    0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
];

#[contract]
impl InputAliasTest for InputAliasTestContract {
    fn test_inputs_at_one_address() {
        let mut arena = Arena::new();
        arena.count_from(A, 1);
        // nonce and contract_hash are the same thirty-two bytes.
        assert_eq!(get_deploy_address(&mut arena, None, A, A, OUT), NONE);
        assert!(arena.get32(OUT) == reference_address(None, 1, 1));
        // Only the output was written.
        assert!(arena.untouched(0, A));
        assert!(arena.untouched(A + 32, OUT - (A + 32)));
        assert!(arena.untouched(OUT + 32, ARENA_LEN - (OUT + 32)));
    }

    fn test_every_input_at_one_address() {
        let mut arena = Arena::new();
        arena.count_from(A, 7);
        // caller_address, an optional input, with nonce and contract_hash.
        assert_eq!(get_deploy_address(&mut arena, Some(A), A, A, OUT), NONE);
        assert!(arena.get32(OUT) == reference_address(Some(7), 7, 7));
    }

    fn test_inputs_overlapping_in_part() {
        let mut arena = Arena::new();
        // nonce at A and contract_hash sixteen bytes into it: the hash's
        // first half is the nonce's second half.
        arena.count_from(A, 1);
        arena.count_from(A + 32, 33);
        assert_eq!(get_deploy_address(&mut arena, None, A, A + 16, OUT), NONE);
        assert!(arena.get32(OUT) == reference_address(None, 1, 17));
    }

    fn test_input_span_inside_input() {
        // A stored value that is the key's own bytes: all of them, then the
        // middle sixteen.
        for (value, len) in [(A, 32usize), (A + 8, 16)] {
            let mut arena = Arena::new();
            arena.count_from(A, 1);
            assert_eq!(bytestring_set(&mut arena, A, value, len), NONE);

            // Read back through buffers that overlap nothing.
            assert_eq!(bytestring_get(&mut arena, A, B, 64, OUT), NONE);
            assert_eq!(arena.u32_at(OUT) as usize, len);
            let first = (1 + value - A) as u8;
            for i in 0..len {
                assert_eq!(arena.bytes[B + i], first + i as u8);
            }
            assert!(arena.untouched(B + len, 64 - len));
        }
    }

    fn test_zero_length_input_span_inside_input() {
        let mut arena = Arena::new();
        arena.count_from(A, 1);
        // An empty value whose pointer is in the middle of the key.
        assert_eq!(bytestring_set(&mut arena, A, A + 8, 0), NONE);

        assert_eq!(bytestring_get(&mut arena, A, B, 64, OUT), NONE);
        assert_eq!(arena.u32_at(OUT), 0);
        assert!(arena.untouched(B, 64));
    }

    fn test_output_overlapping_input() {
        // to_be_deployed on the nonce, on each of its end bytes, on the
        // contract hash, and on the optional caller address.
        for (caller, out) in [
            (None, B),
            (None, B + 31),
            (None, B - 31),
            (None, C),
            (Some(A), A + 31),
        ] {
            let mut arena = Arena::new();
            assert_eq!(
                get_deploy_address(&mut arena, caller, B, C, out),
                FAULT
            );
            assert!(arena.untouched(0, ARENA_LEN));
        }

        // On an input that two arguments share.
        let mut arena = Arena::new();
        assert_eq!(get_deploy_address(&mut arena, None, A, A, A + 16), FAULT);
        assert!(arena.untouched(0, ARENA_LEN));
    }

    fn test_output_overlapping_output() {
        // GetCalldataLen() -> len, auth_token_count: two four-byte outputs,
        // at one address and sharing a single byte either way round.
        for (len, count) in [(OUT, OUT), (OUT, OUT + 3), (OUT + 3, OUT)] {
            let mut arena = Arena::new();
            let words = [arena.at(len), arena.at(count)];
            assert_eq!(syscall(SyscallNo::kGetCalldataLen, words), FAULT);
            assert!(arena.untouched(0, ARENA_LEN));
        }
    }

    fn test_zero_length_input_span_against_output() {
        // SHA256(buffer) -> hash, hashing no bytes.
        let mut arena = Arena::new();
        let words = [arena.at(OUT + 8), 0, arena.at(OUT)];
        assert_eq!(syscall(SyscallNo::kSHA256, words), FAULT);
        assert!(arena.untouched(0, ARENA_LEN));

        for edge in [OUT, OUT + 32] {
            let mut arena = Arena::new();
            let words = [arena.at(edge), 0, arena.at(OUT)];
            assert_eq!(syscall(SyscallNo::kSHA256, words), NONE);
            assert!(arena.get32(OUT) == EMPTY_SHA256);
        }
    }

    fn test_zero_length_output_span_against_input() {
        // BytestringGet(key) -> bytestring, written_len, into no bytes.
        let mut arena = Arena::new();
        assert_eq!(bytestring_get(&mut arena, A, A + 8, 0, OUT), FAULT);
        assert!(arena.untouched(0, ARENA_LEN));

        for edge in [A, A + 32] {
            let mut arena = Arena::new();
            // The arguments pass. What the body then says about a key nobody
            // wrote is not this method's business.
            assert_ne!(bytestring_get(&mut arena, A, edge, 0, OUT), FAULT);
        }
    }

    fn test_touching_ranges() {
        let mut arena = Arena::new();
        // nonce, contract_hash and to_be_deployed back to back.
        arena.count_from(A, 1);
        arena.count_from(A + 32, 33);
        assert_eq!(
            get_deploy_address(&mut arena, None, A, A + 32, A + 64),
            NONE
        );
        assert!(arena.get32(A + 64) == reference_address(None, 1, 33));
    }
}
