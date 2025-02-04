#![allow(non_upper_case_globals)]

use crate::ffi_enums::HostFnErrorCode;
use core::ffi::c_void;
use wasmi::core::HostError;
use wasmi::Error;

use core::fmt;

/**
 * Error policy:
 * Syscalls should not be handled as "errors" returned from the syscall;
 * instead, syscalls return a uint64 that acts as the error code
 * (which the SDK can then check and call `unreachable`).
 *
 * There are two types of errors that we are concerned with here.
 * 1) System errors (which could be nondeterministic, i.e. malloc fail)
 * at which point we need to abort the whole transaction.
 * 2) OutOfGas errors, within the syscall.  We'd like to just return
 * an OutOfGas exception into wasmi, but wasmi does not expose
 * that as a public API, so we return a different error instead if
 * a syscall causes an OutOfGas error.
 * (The syscall will set the fuel to zero before returning in that case.
 * However, we do not have a guarantee that another instruction will execute
 * before the wasm function that called the syscall returns).
 * I'm using Wasmi's HostError API for this extra type of error.
 *
 */

#[allow(dead_code)]
#[repr(i32)]
#[derive(Clone, Copy, Debug)]
pub enum TrampolineErrorEnum {
    OutOfGas = 1,
    Unrecoverable = 2,
    Return = 3,
    Exit { code: i32 } = 4,
    ValidationFailure = 5,
}

#[repr(C)]
pub struct TrampolineResult {
    result: i32,
    panic: i32,
}

#[derive(Debug, Copy, Clone)]
pub struct TrampolineError {
    pub error: TrampolineErrorEnum,
}

impl fmt::Display for TrampolineError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "TrampolineError error printing is annoying")
    }
}

impl HostError for TrampolineError {}

extern "C" {
    pub fn c_call_0args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
    ) -> TrampolineResult;
    pub fn c_call_1args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
    ) -> TrampolineResult;
    pub fn c_call_2args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
    ) -> TrampolineResult;
    pub fn c_call_3args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
        arg3: u64,
    ) -> TrampolineResult;
    pub fn c_call_4args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
    ) -> TrampolineResult;
    pub fn c_call_5args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
    ) -> TrampolineResult;
    pub fn c_call_6args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
        arg6: u64,
    ) -> TrampolineResult;
    pub fn c_call_7args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
        arg6: u64,
        arg7: u64,
    ) -> TrampolineResult;
    pub fn c_call_8args(
        fn_pointer: *mut c_void,
        userctx: *mut c_void,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
        arg6: u64,
        arg7: u64,
        arg8: u64,
    ) -> TrampolineResult;
}

// This is annoying, but I'm not sure how better to deal with this
pub fn handle_trampoline_error(result: TrampolineResult) -> Result<i32, Error> {
    match result.panic {
        x if x == HostFnErrorCode::kNoneOrRecoverable as i32 => {
            Ok(result.result)
        }
        x if x == HostFnErrorCode::kOutOfGas as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::OutOfGas,
            }))
        }
        x if x == HostFnErrorCode::kReturn as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::Return,
            }))
        }
        x if x == HostFnErrorCode::kExit as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::Exit {
                    code: result.result,
                },
            }))
        }
        x if x == HostFnErrorCode::kValidationFailure as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::ValidationFailure,
            }))
        }
        _ => Err(Error::host(TrampolineError {
            error: TrampolineErrorEnum::Unrecoverable,
        })),
    }
}
