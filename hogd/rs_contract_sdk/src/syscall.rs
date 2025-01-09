use rs_xdr::SyscallNo;

#[link(wasm_import_module = "marmot")]
extern "C" {
    pub(crate) fn syscall(
        syscall: u64,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
        arg6: u64,
    ) -> i32;
}

pub fn syscall_wrapper(
    syscall_no: SyscallNo,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
) -> i32 {
    unsafe { syscall(syscall_no as u64, arg1, arg2, arg3, arg4, arg5, arg6) }
}

pub(crate) fn to_offset_mut<T>(obj: &mut T) -> *mut u8 {
    unsafe { core::mem::transmute(obj) }
}

pub(crate) fn to_offset<T>(obj: &T) -> *const u8 {
    unsafe { core::mem::transmute(obj) }
}
