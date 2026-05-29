#![no_main]
#![no_std]

use core::arch::asm;

const SYSCALL_SHA256: usize = 0;
const SYSCALL_RETURN: usize = 6;
const HASH_BYTES: usize = 32;

type EntryPoint = unsafe extern "C" fn() -> !;

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

#[no_mangle]
#[link_section = ".text._start"]
pub unsafe extern "C" fn _start() -> ! {
    sha256_return_hash()
}

#[no_mangle]
#[link_section = ".text.sha256_repeated"]
pub unsafe extern "C" fn RV32_COMPILED_SHA256_RETURN_SHA256_REPEATED() -> ! {
    sha256_return_empty()
}

#[used]
#[link_section = ".rodata.entry_points"]
static RV32_COMPILED_SHA256_RETURN_ENTRY_POINTS: [EntryPoint; 2] =
    [_start, RV32_COMPILED_SHA256_RETURN_SHA256_REPEATED];

#[inline(always)]
unsafe fn sha256_return_hash() -> ! {
    let input_ptr = RV32_COMPILED_SHA256_RETURN_INPUT.as_ptr() as usize;
    let output_ptr = core::ptr::addr_of_mut!(SHA_OUTPUT) as *mut u8 as usize;

    syscall(SYSCALL_SHA256, input_ptr,
            RV32_COMPILED_SHA256_RETURN_INPUT.len(), output_ptr, 0, 0);
    syscall(SYSCALL_RETURN, output_ptr, HASH_BYTES, 0, 0, 0);

    loop {
        asm!("", options(nomem, nostack));
    }
}

#[inline(always)]
unsafe fn sha256_return_empty() -> ! {
    let input_ptr = RV32_COMPILED_SHA256_RETURN_INPUT.as_ptr() as usize;
    let output_ptr = core::ptr::addr_of_mut!(SHA_OUTPUT) as *mut u8 as usize;

    syscall(SYSCALL_SHA256, input_ptr,
            RV32_COMPILED_SHA256_RETURN_INPUT.len(), output_ptr, 0, 0);
    syscall(SYSCALL_RETURN, 0, 0, 0, 0, 0);

    loop {
        asm!("", options(nomem, nostack));
    }
}

// Register-native syscall ABI: the syscall number is passed in a0 and arguments
// in a1..a5 (the bridge sign-extends each register). The syscall return code is
// delivered in a0; output buffers are written through the guest pointers passed
// in registers, so there is no result frame to read back.
#[inline(always)]
unsafe fn syscall(num: usize, a1: usize, a2: usize, a3: usize, a4: usize,
                  a5: usize) -> usize {
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
