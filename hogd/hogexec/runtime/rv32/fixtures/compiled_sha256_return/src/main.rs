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

// v4 guest-side dispatch. The runtime enters at _start (PC 0) with the invoked
// method id seeded into a0 (low 32 bits) / a1 (high 32 bits). _start reads them
// before any prologue runs, compares against the two method selectors, and tail
// -calls the matching body; an unknown id falls through to `ebreak`, a
// deterministic trap (the dispatcher's default arm). The selector low/high
// halves below must match SHA256_METHOD_ID / SHA256_REPEATED_METHOD_ID in
// lower_elf.py and the Sha256Wrapper entrypoint hashes the C++ parity test
// pins.
const SHA256_LO: i32 = 0xA719_DD56_u32 as i32; // SHA256_METHOD_ID & 0xFFFF_FFFF
const SHA256_HI: i32 = 0xCA57_1C66_u32 as i32; // SHA256_METHOD_ID >> 32
const SHA256_REPEATED_LO: i32 = 0x5D1F_3CFE_u32 as i32;
const SHA256_REPEATED_HI: i32 = 0x5749_E8FA_u32 as i32;

global_asm!(
    ".pushsection .text._start,\"ax\",@progbits",
    ".globl _start",
    ".type _start, @function",
    "_start:",
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
    "  ebreak",
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
