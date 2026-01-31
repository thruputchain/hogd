#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

use core::ffi::c_void;

use crate::{HostFnErrorCode, InvokeErrorCode};

#[repr(C)]
pub struct TrampolineResult {
    // if HostFnErrorCode is kNoneOrRecoverable, this is the SyscallErrorCode.
    // Otherwise, this will be 0.
    pub result: i32,
    // Corresponds to HostFnErrorCode.
    pub panic: i32,
}

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
pub type TrampolineCall0Args =
    unsafe extern "C" fn(host_call_context: *mut c_void) -> TrampolineResult;

pub type TrampolineCall1Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
) -> TrampolineResult;

pub type TrampolineCall2Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
) -> TrampolineResult;

pub type TrampolineCall3Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
) -> TrampolineResult;

pub type TrampolineCall4Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
) -> TrampolineResult;

pub type TrampolineCall5Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
) -> TrampolineResult;

pub type TrampolineCall6Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
) -> TrampolineResult;

pub type TrampolineCall7Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
    arg7: u64,
) -> TrampolineResult;

pub type TrampolineCall8Args = unsafe extern "C" fn(
    host_call_context: *mut c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
    arg7: u64,
    arg8: u64,
) -> TrampolineResult;

pub fn c_call_0args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall0Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context)
    }
}

pub fn c_call_1args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall1Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context, arg1)
    }
}

pub fn c_call_2args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall2Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context, arg1, arg2)
    }
}

pub fn c_call_3args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall3Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context, arg1, arg2, arg3)
    }
}

pub fn c_call_4args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall4Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context, arg1, arg2, arg3, arg4)
    }
}

pub fn c_call_5args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall5Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context, arg1, arg2, arg3, arg4, arg5)
    }
}

pub fn c_call_6args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall6Args,
        >(function_pointer);
        reinterpreted_ptr(host_call_context, arg1, arg2, arg3, arg4, arg5, arg6)
    }
}

pub fn c_call_7args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
    arg7: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall7Args,
        >(function_pointer);
        reinterpreted_ptr(
            host_call_context,
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            arg6,
            arg7,
        )
    }
}

pub fn c_call_8args(
    function_pointer: *mut ::std::os::raw::c_void,
    host_call_context: *mut ::std::os::raw::c_void,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    arg4: u64,
    arg5: u64,
    arg6: u64,
    arg7: u64,
    arg8: u64,
) -> TrampolineResult {
    unsafe {
        let reinterpreted_ptr = std::mem::transmute::<
            *mut c_void,
            TrampolineCall8Args,
        >(function_pointer);
        reinterpreted_ptr(
            host_call_context,
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            arg6,
            arg7,
            arg8,
        )
    }
}
