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

impl Into<Result<i32, HostFnErrorCode>> for TrampolineResult {
    fn into(self) -> Result<i32, HostFnErrorCode> {
        let panic: HostFnErrorCode = {
            match self.panic.try_into() {
                Ok(panic) => panic,
                Err(_) => return Err(HostFnErrorCode::Unrecoverable),
            }
        };

        match panic {
            HostFnErrorCode::NoneOrRecoverable => Ok(self.result),
            _ => Err(panic),
        }
    }
}

// if a TrampolineError gets thrown within a wasm invocation,
// this error is caught at the root of the invocation and converted to an InvokeResult.

pub fn to_invoke_result(error: HostFnErrorCode) -> Result<(), InvokeErrorCode> {
    debug_assert!(error != HostFnErrorCode::NoneOrRecoverable);
    match error {
        HostFnErrorCode::OutOfGas => Err(InvokeErrorCode::OutOfGas),
        HostFnErrorCode::Return => Ok(()),
        HostFnErrorCode::Exit => {
            Err(InvokeErrorCode::Deterministic)
        }
        HostFnErrorCode::ValidationFailure => {
            Err(InvokeErrorCode::ValidationFailure)
        }
        HostFnErrorCode::Abandon => {
            Err(InvokeErrorCode::Abandon)
        }
        _ => Err(InvokeErrorCode::Unrecoverable),
    }
}
macro_rules! define_trampoline {
    ($type_name:ident, $fn_name:ident, $($arg:ident),*) => {
        pub type $type_name = unsafe extern "C" fn(
            host_call_context: *mut c_void,
            $($arg: u64),*
        ) -> TrampolineResult;

        pub fn $fn_name(
            function_pointer: crate::ErasedFnPtr,
            host_call_context: *mut ::std::os::raw::c_void,
            $($arg: u64),*
        ) -> TrampolineResult {
            // fn-ptr to fn-ptr transmute is well-defined in Rust
            // (all fn pointers have the same size and representation).
            unsafe {
                let f = std::mem::transmute::<crate::ErasedFnPtr, $type_name>(function_pointer);
                f(host_call_context, $($arg),*)
            }
        }
    };
}

define_trampoline!(TrampolineCall0Args, c_call_0args,);
define_trampoline!(TrampolineCall1Args, c_call_1args, arg1);
define_trampoline!(TrampolineCall2Args, c_call_2args, arg1, arg2);
define_trampoline!(TrampolineCall3Args, c_call_3args, arg1, arg2, arg3);
define_trampoline!(TrampolineCall4Args, c_call_4args, arg1, arg2, arg3, arg4);
define_trampoline!(
    TrampolineCall5Args,
    c_call_5args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5
);
define_trampoline!(
    TrampolineCall6Args,
    c_call_6args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6
);
define_trampoline!(
    TrampolineCall7Args,
    c_call_7args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6,
    arg7
);
define_trampoline!(
    TrampolineCall8Args,
    c_call_8args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6,
    arg7,
    arg8
);
define_trampoline!(
    TrampolineCall9Args,
    c_call_9args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6,
    arg7,
    arg8,
    arg9
);
define_trampoline!(
    TrampolineCall10Args,
    c_call_10args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6,
    arg7,
    arg8,
    arg9,
    arg10
);
define_trampoline!(
    TrampolineCall11Args,
    c_call_11args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6,
    arg7,
    arg8,
    arg9,
    arg10,
    arg11
);
define_trampoline!(
    TrampolineCall12Args,
    c_call_12args,
    arg1,
    arg2,
    arg3,
    arg4,
    arg5,
    arg6,
    arg7,
    arg8,
    arg9,
    arg10,
    arg11,
    arg12
);
