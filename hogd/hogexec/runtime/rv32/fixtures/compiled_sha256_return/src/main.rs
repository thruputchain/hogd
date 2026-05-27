#![no_main]
#![no_std]

use core::arch::asm;

const SYSCALL_SHA256: u64 = 0;
const SYSCALL_RETURN: u64 = 6;
const SYSCALL_BRIDGE_VERSION: usize = 0;
const HASH_BYTES: usize = 32;

#[no_mangle]
#[link_section = ".rodata.fixture_input"]
static RV32_COMPILED_SHA256_RETURN_INPUT: [u8; 7] = [1, 2, 3, 4, 5, 8, 13];

#[repr(C, align(8))]
struct SyscallFrame {
    words: [u64; 11],
    result_code: i32,
    panic: i32,
    gas_consumed: u64,
}

impl SyscallFrame {
    const fn zero() -> Self {
        Self {
            words: [0; 11],
            result_code: 0,
            panic: 0,
            gas_consumed: 0,
        }
    }
}

#[link_section = ".bss.sha_frame"]
static mut SHA_FRAME: SyscallFrame = SyscallFrame::zero();

#[link_section = ".bss.return_frame"]
static mut RETURN_FRAME: SyscallFrame = SyscallFrame::zero();

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

#[inline(always)]
unsafe fn sha256_return_hash() -> ! {
    let input_ptr = RV32_COMPILED_SHA256_RETURN_INPUT.as_ptr() as usize;
    let output_ptr = core::ptr::addr_of_mut!(SHA_OUTPUT) as *mut u8 as usize;

    let sha_frame = core::ptr::addr_of_mut!(SHA_FRAME);
    (*sha_frame).words[0] = SYSCALL_SHA256;
    (*sha_frame).words[1] = input_ptr as u64;
    (*sha_frame).words[2] = RV32_COMPILED_SHA256_RETURN_INPUT.len() as u64;
    (*sha_frame).words[3] = output_ptr as u64;
    issue_syscall(sha_frame);

    let return_frame = core::ptr::addr_of_mut!(RETURN_FRAME);
    (*return_frame).words[0] = SYSCALL_RETURN;
    (*return_frame).words[1] = output_ptr as u64;
    (*return_frame).words[2] = HASH_BYTES as u64;
    issue_syscall(return_frame);

    loop {
        asm!("", options(nomem, nostack));
    }
}

#[inline(always)]
unsafe fn sha256_return_empty() -> ! {
    let input_ptr = RV32_COMPILED_SHA256_RETURN_INPUT.as_ptr() as usize;
    let output_ptr = core::ptr::addr_of_mut!(SHA_OUTPUT) as *mut u8 as usize;

    let sha_frame = core::ptr::addr_of_mut!(SHA_FRAME);
    (*sha_frame).words[0] = SYSCALL_SHA256;
    (*sha_frame).words[1] = input_ptr as u64;
    (*sha_frame).words[2] = RV32_COMPILED_SHA256_RETURN_INPUT.len() as u64;
    (*sha_frame).words[3] = output_ptr as u64;
    issue_syscall(sha_frame);

    let return_frame = core::ptr::addr_of_mut!(RETURN_FRAME);
    (*return_frame).words[0] = SYSCALL_RETURN;
    (*return_frame).words[1] = 0;
    (*return_frame).words[2] = 0;
    issue_syscall(return_frame);

    loop {
        asm!("", options(nomem, nostack));
    }
}

#[inline(always)]
unsafe fn issue_syscall(frame: *mut SyscallFrame) -> usize {
    let frame_ptr = frame as usize;
    let result: usize;
    asm!(
        "ecall",
        inlateout("a0") frame_ptr => result,
        lateout("a1") _,
        lateout("a2") _,
        lateout("a3") _,
        lateout("a4") _,
        lateout("a5") _,
        lateout("a6") _,
        inlateout("a7") SYSCALL_BRIDGE_VERSION => _,
        options(nostack),
    );

    let _footer_result_code = core::ptr::read_volatile(core::ptr::addr_of!((*frame).result_code));
    let _footer_panic = core::ptr::read_volatile(core::ptr::addr_of!((*frame).panic));
    let _footer_gas = core::ptr::read_volatile(core::ptr::addr_of!((*frame).gas_consumed));
    result
}
