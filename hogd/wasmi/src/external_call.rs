use core::ffi::c_void;
use wasmi::Error;
use wasmi::core::HostError;

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
#[repr(u8)]
enum TrampolineErrorEnum {
    None = 0,
    OutOfGasError = 1,
    UnrecoverableSystemError = 2,
}

#[repr(C)]
pub struct TrampolineResult {
    result: u64,
    panic: u8,
}

#[derive(Debug, Copy, Clone)]
pub struct TrampolineError {
    pub recoverable : bool
}

impl fmt::Display for TrampolineError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "TrampolineError recoverable={}" , self.recoverable)
    }
}

impl HostError for TrampolineError { }


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
}

// This is annoying, but I'm not sure how better to deal with this
pub fn handle_trampoline_error(result: TrampolineResult) -> Result<u64, Error> {
    let err: TrampolineErrorEnum = unsafe { std::mem::transmute(result.panic) };
    match err {
        TrampolineErrorEnum::None => Ok(result.result),
        TrampolineErrorEnum::OutOfGasError => Err(Error::host(TrampolineError{ recoverable : true })),
        _ => Err(Error::host(TrampolineError {recoverable : false})),
    }
}
