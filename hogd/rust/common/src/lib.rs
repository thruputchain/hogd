#[rustfmt::skip]
mod ffi_enums;
pub use crate::ffi_enums::*;

pub mod trampoline_bindings;
pub use trampoline_bindings::*;

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
