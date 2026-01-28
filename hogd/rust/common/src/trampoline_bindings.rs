#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use crate::{HostFnErrorCode, InvokeErrorCode};

// if a TrampolineError gets thrown within a wasm invocation,
// this error is caught at the root of the invocation and converted to an InvokeResult.

pub fn to_invoke_result(error: HostFnErrorCode) -> Result<(), InvokeErrorCode> {
    debug_assert!(error != HostFnErrorCode::kHostNoneOrRecoverable);
    match error {
        HostFnErrorCode::kHostOutOfGas => Err(InvokeErrorCode::kInvokeOutOfGas),
        HostFnErrorCode::kHostReturn => Ok(()),
        HostFnErrorCode::kHostExit => {
            Err(InvokeErrorCode::kInvokeDeterministic)
        }
        HostFnErrorCode::kHostValidationFailure => {
            Err(InvokeErrorCode::kInvokeValidationFailure)
        }
        HostFnErrorCode::kHostAbortTransaction => {
            Err(InvokeErrorCode::kInvokeAbortTransaction)
        }
        _ => Err(InvokeErrorCode::kInvokeUnrecoverable),
    }
}
