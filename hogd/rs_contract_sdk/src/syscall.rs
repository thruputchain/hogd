
use rs_xdr::SyscallNo;

#[link(wasm_import_module = "marmot")]
extern "C" {
    #[link_name = "syscall"]
    pub (crate) fn syscall_internal(syscall: u64, arg1: u64, arg2: u64, arg3: u64, arg4: u64) -> i32;
}

pub fn syscall(syscall: SyscallNo, arg1: u64, arg2: u64, arg3: u64, arg4: u64) -> i32 {
    unsafe { syscall_internal(syscall as u64, arg1, arg2, arg3, arg4) }
}

pub(crate) fn to_offset_mut<T>(obj: &mut T) -> *mut u8 {
    unsafe { core::mem::transmute(&obj) }
}

