#![no_main]
#![no_std]

use core::arch::{asm, global_asm};

const SYSCALL_SHA256: usize = 0;
const SYSCALL_RETURN: usize = 6;
const HASH_BYTES: usize = 32;

#[no_mangle]
#[link_section = ".rodata.fixture_input"]
static RV32_COMPILED_SHA256_RETURN_INPUT: [u8; 7] = [1, 2, 3, 4, 5, 8, 13];

#[link_section = ".bss.sha_output"]
static mut SHA_OUTPUT: [u8; HASH_BYTES] = [0; HASH_BYTES];

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        unsafe {
            asm!("", options(nomem, nostack));
        }
    }
}

// Guest-side dispatch. Entry ABI v3 enters at _start (PC 0) with sp = 0 (the
// guest owns stack placement); _start establishes sp at the top of the 64K data
// space (0x10000) before dispatch. The method id is seeded into a0 (low 32
// bits) / a1 (high 32 bits); _start compares against the two method selectors
// and tail-calls the matching body; an unknown id falls through to canonical
// UNIMP, a deterministic invalid-instruction trap. The selector low/high halves
// below must match SHA256_METHOD_ID / SHA256_REPEATED_METHOD_ID in
// package_artifact.py and the Sha256Wrapper entrypoint hashes the C++ parity
// test pins.
const SHA256_LO: i32 = 0xFC9E_81B9_u32 as i32; // SHA256_METHOD_ID & 0xFFFF_FFFF
const SHA256_HI: i32 = 0xE8CC_FC8F_u32 as i32; // SHA256_METHOD_ID >> 32
const SHA256_REPEATED_LO: i32 = 0x70E3_8729_u32 as i32;
const SHA256_REPEATED_HI: i32 = 0x2F2C_B892_u32 as i32;

global_asm!(
    ".pushsection .text._start,\"ax\",@progbits",
    ".globl _start",
    ".type _start, @function",
    "_start:",
    // Entry ABI v3 seeds sp = 0 (the guest owns stack placement): establish
    // the stack at the top of the 64K data space before anything else runs.
    "  lui  sp, 16",
    "  li   t0, {sha_lo}",
    "  bne  a0, t0, 1f",
    "  li   t0, {sha_hi}",
    "  bne  a1, t0, 1f",
    "  tail {sha}",
    "1:",
    "  li   t0, {rep_lo}",
    "  bne  a0, t0, 2f",
    "  li   t0, {rep_hi}",
    "  bne  a1, t0, 2f",
    "  tail {rep}",
    "2:",
    "  .word 0xc0001073",
    ".popsection",
    sha = sym sha256_return_hash,
    rep = sym sha256_return_empty,
    sha_lo = const SHA256_LO,
    sha_hi = const SHA256_HI,
    rep_lo = const SHA256_REPEATED_LO,
    rep_hi = const SHA256_REPEATED_HI,
);

#[no_mangle]
#[link_section = ".text.sha256_hash"]
unsafe extern "C" fn sha256_return_hash() -> ! {
    let input_ptr = RV32_COMPILED_SHA256_RETURN_INPUT.as_ptr() as usize;
    let output_ptr = core::ptr::addr_of_mut!(SHA_OUTPUT) as *mut u8 as usize;

    syscall(
        SYSCALL_SHA256,
        input_ptr,
        RV32_COMPILED_SHA256_RETURN_INPUT.len(),
        output_ptr,
        0,
        0,
    );
    syscall(SYSCALL_RETURN, output_ptr, HASH_BYTES, 0, 0, 0);

    loop {
        asm!("", options(nomem, nostack));
    }
}

#[no_mangle]
#[link_section = ".text.sha256_repeated"]
unsafe extern "C" fn sha256_return_empty() -> ! {
    let input_ptr = RV32_COMPILED_SHA256_RETURN_INPUT.as_ptr() as usize;
    let output_ptr = core::ptr::addr_of_mut!(SHA_OUTPUT) as *mut u8 as usize;

    syscall(
        SYSCALL_SHA256,
        input_ptr,
        RV32_COMPILED_SHA256_RETURN_INPUT.len(),
        output_ptr,
        0,
        0,
    );
    syscall(SYSCALL_RETURN, 0, 0, 0, 0, 0);

    loop {
        asm!("", options(nomem, nostack));
    }
}

// Register-native syscall ABI: the syscall number is passed in a0 and arguments
// in a1..a5. The syscall return code is delivered in a0; output buffers are
// written through the guest pointers passed in registers, so there is no result
// frame to read back.
#[inline(always)]
unsafe fn syscall(
    num: usize,
    a1: usize,
    a2: usize,
    a3: usize,
    a4: usize,
    a5: usize,
) -> usize {
    let result: usize;
    asm!(
        "ecall",
        inlateout("a0") num => result,
        inlateout("a1") a1 => _,
        inlateout("a2") a2 => _,
        inlateout("a3") a3 => _,
        inlateout("a4") a4 => _,
        inlateout("a5") a5 => _,
        lateout("a6") _,
        lateout("a7") _,
        options(nostack),
    );
    result
}
