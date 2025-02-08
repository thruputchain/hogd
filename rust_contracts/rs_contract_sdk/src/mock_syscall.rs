// using HostFnResult = std::pair<HostFnErrorCode, int32_t>;

use groundhog_xdr::HostFnErrorCode;

use crate::exit_error;
use crate::out_of_gas_error;
use crate::return_error;
use crate::unrecoverable_error;
use crate::validation_error;
use std::panic::panic_any;

// https://blog.rust-lang.org/2024/03/30/i128-layout-update.html

#[repr(C)]
struct FFIHostFnResult {
    result: i32,
    host_fn_panic: i32,
}

impl FFIHostFnResult {
    pub fn unwrap(&self) -> i32 {
        match self.host_fn_panic {
            x if x == HostFnErrorCode::kNoneOrRecoverable as i32 => {
                return self.result;
            }
            x if x == HostFnErrorCode::kOutOfGas as i32 => {
                panic_any(out_of_gas_error());
            }
            x if x == HostFnErrorCode::kUnrecoverable as i32 => {
                panic_any(unrecoverable_error());
            }
            x if x == HostFnErrorCode::kReturn as i32 => {
                panic_any(return_error());
            }
            x if x == HostFnErrorCode::kExit as i32 => {
                panic_any(exit_error(self.result));
            }
            x if x == HostFnErrorCode::kValidationFailure as i32 => {
                panic_any(validation_error());
            }
            _ => {
                panic_any(unrecoverable_error());
            }
        }
    }
}
