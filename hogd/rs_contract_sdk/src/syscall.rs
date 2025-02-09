pub use ::rs_xdr::syscall::{SyscallNo, SyscallErrorCode};

/// Rust bindings for the syscalls
pub mod bindings;

// provides TryInto<SyscallErrorCode> for SysErr
mod error_helper;

use thiserror::Error;
#[derive(Error, Copy, Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize)]
#[error("SysErr({0})")]
pub struct SysErr(i32);

impl SysErr
{
    pub fn is_code(self, code: SyscallErrorCode) -> bool {
        if let Ok(c) = TryInto::<SyscallErrorCode>::try_into(self) {
            c == code
        } else {
            false
        }
    }

    pub fn is_none(self) -> bool {
        self.is_code(SyscallErrorCode::kNone)
    }

    pub fn is_some(self) -> bool {
        !self.is_none()
    }
}

impl From<SyscallErrorCode> for SysErr {
    fn from(code: SyscallErrorCode) -> Self {
        SysErr(code as i32)
    }
}

impl Into<i32> for SysErr {
    fn into(self) -> i32 {
        self.0
    }
}

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
) -> SysErr {
    unsafe {
        let errno = syscall(syscall_no as u64, arg1, arg2, arg3, arg4, arg5, arg6);
        SysErr(errno)
    }
}

pub(crate) fn to_offset_mut<T>(obj: &mut T) -> *mut u8 {
    unsafe { core::mem::transmute(obj) }
}

pub(crate) fn to_offset<T>(obj: &T) -> *const u8 {
    unsafe { core::mem::transmute(obj) }
}
