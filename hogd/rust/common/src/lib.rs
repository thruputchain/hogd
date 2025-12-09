#[rustfmt::skip]
mod ffi_enums;
pub use crate::ffi_enums::*;

pub mod trampoline_bindings;
pub use trampoline_bindings::*;

use core::slice;
use core::str::Utf8Error;
use std::result::Result;

impl From<SystemErrorCode> for HostFnErrorCode {
    fn from(error_code: SystemErrorCode) -> Self {
        match error_code {
            SystemErrorCode::kSystemUnrecoverable => {
                HostFnErrorCode::kHostUnrecoverable
            }
            SystemErrorCode::kSystemAbortTransaction => {
                HostFnErrorCode::kHostAbortTransaction
            }
            SystemErrorCode::kSystemValidationFailure => {
                HostFnErrorCode::kHostValidationFailure
            }
        }
    }
}

impl From<SystemErrorCode> for InvokeErrorCode {
    fn from(error_code: SystemErrorCode) -> Self {
        match error_code {
            SystemErrorCode::kSystemUnrecoverable => {
                InvokeErrorCode::kInvokeUnrecoverable
            }
            SystemErrorCode::kSystemAbortTransaction => {
                InvokeErrorCode::kInvokeAbortTransaction
            }
            SystemErrorCode::kSystemValidationFailure => {
                InvokeErrorCode::kInvokeValidationFailure
            }
        }
    }
}

pub fn string_from_parts(
    bytes: *const u8,
    len: u32,
) -> Result<String, Utf8Error> {
    let slice = unsafe { slice::from_raw_parts(bytes, len as usize) };
    std::str::from_utf8(slice).map(|s| s.to_owned())
}
