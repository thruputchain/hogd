use core::ffi::c_void;
use wasmi::Error;

/**
 * Error policy:
 * This does not implement transferral of errors
 * from the syscall to the wasm runtime in the full granularity
 * of the rest of the hogd codebase.
 *
 * There are two types of errors that we are concerned with here.
 * 1) Those that should be returned to the Wasm runtime (and that could
 * be handled there, by a contract)
 * 2) All other errors
 */

#[allow(dead_code)]
#[repr(u8)]
enum TrampolineError {
    None = 0,
    HostError = 1,
    UnrecoverableSystemError = 2,
}

#[repr(C)]
pub struct TrampolineResult {
    result: u64,
    panic: u8,
}

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
    let err: TrampolineError = unsafe { std::mem::transmute(result.panic) };
    match err {
        TrampolineError::None => Ok(result.result),
        TrampolineError::HostError => Err(Error::new("HostError")),
        _ => Err(Error::new("UnrecoverableSystemError")),
    }
}
