#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use core::fmt;
use wasmi::core::HostError;
use wasmi::Error;

use crate::invoke_result::WasmiInvokeResult;
use common::{HostFnErrorCode, InvokeErrorCode};

#[derive(Debug, Copy, Clone)]
pub struct TrampolineError {
    pub error: HostFnErrorCode,
}

impl fmt::Display for TrampolineError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "TrampolineError: {:?}", self.error)
    }
}

impl TrampolineError {
    // if a TrampolineError gets thrown within the wasmi invocation,
    // this error is caught at the root of the invocation and converted to an InvokeResult.
    pub fn to_wasmi_invoke_result(self) -> WasmiInvokeResult {
        debug_assert!(self.error != HostFnErrorCode::kHostNoneOrRecoverable);
        match self.error {
            HostFnErrorCode::kHostOutOfGas => {
                WasmiInvokeResult::error(InvokeErrorCode::kInvokeOutOfGas)
            }
            HostFnErrorCode::kHostReturn => WasmiInvokeResult::success(),
            HostFnErrorCode::kHostExit => {
                WasmiInvokeResult::error(InvokeErrorCode::kInvokeDeterministic)
            }
            HostFnErrorCode::kHostValidationFailure => {
                WasmiInvokeResult::error(
                    InvokeErrorCode::kInvokeValidationFailure,
                )
            }
            HostFnErrorCode::kHostAbortTransaction => WasmiInvokeResult::error(
                InvokeErrorCode::kInvokeAbortTransaction,
            ),
            _ => {
                WasmiInvokeResult::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
        }
    }
}

impl HostError for TrampolineError {}

// This is annoying, but I'm not sure how better to deal with this
pub fn handle_trampoline_error(result: TrampolineResult) -> Result<i32, Error> {
    let error = result.panic.try_into();
    if error.is_err() {
        return Err(Error::host(TrampolineError {
            error: HostFnErrorCode::kHostUnrecoverable,
        }));
    }
    let error = error.unwrap();
    match error {
        HostFnErrorCode::kHostNoneOrRecoverable => Ok(result.result),
        _ => Err(Error::host(TrampolineError { error })),
    }
}
