#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use crate::{HostFnErrorCode, InvokeErrorCode};

impl Into<(Result<i32, HostFnErrorCode>, u64)> for TrampolineResult {
    fn into(self) -> (Result<i32, HostFnErrorCode>, u64) {
        let panic: HostFnErrorCode = {
            match self.panic.try_into() {
                Ok(panic) => panic,
                Err(_) => {
                    return (
                        Err(HostFnErrorCode::Unrecoverable),
                        self.gas_consumed,
                    )
                }
            }
        };

        match panic {
            HostFnErrorCode::NoneOrRecoverable => {
                (Ok(self.result_code), self.gas_consumed)
            }
            _ => (Err(panic), self.gas_consumed),
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
        HostFnErrorCode::Exit => Err(InvokeErrorCode::Deterministic),
        HostFnErrorCode::ValidationFailure => {
            Err(InvokeErrorCode::ValidationFailure)
        }
        HostFnErrorCode::Abandon => Err(InvokeErrorCode::Abandon),
        _ => Err(InvokeErrorCode::Unrecoverable),
    }
}
macro_rules! define_trampoline {
    ($type_name:ident, $fn_name:ident, $($arg:ident),*) => {

        pub fn $fn_name(
            function_pointer: crate::ErasedFnPtr,
            host_call_context: *mut ::std::os::raw::c_void,
            contract_memory: crate::WasmContext,
            $($arg: u32),*
        ) -> TrampolineResult {
            // fn-ptr to fn-ptr transmute is well-defined in Rust
            // (all fn pointers have the same size and representation).
            unsafe {
                let f = std::mem::transmute::<crate::ErasedFnPtr, $type_name>(function_pointer).unwrap();
                f(host_call_context, contract_memory, $($arg),*)
            }
        }
    };
}

define_trampoline!(HostFn0, c_call_0args,);
define_trampoline!(HostFn1, c_call_1args, arg1);
define_trampoline!(HostFn2, c_call_2args, arg1, arg2);
define_trampoline!(HostFn3, c_call_3args, arg1, arg2, arg3);
define_trampoline!(HostFn4, c_call_4args, arg1, arg2, arg3, arg4);
define_trampoline!(HostFn5, c_call_5args, arg1, arg2, arg3, arg4, arg5);
define_trampoline!(HostFn6, c_call_6args, arg1, arg2, arg3, arg4, arg5, arg6);
define_trampoline!(
    HostFn7,
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
    HostFn8,
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
    HostFn9,
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
    HostFn10,
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
    HostFn11,
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
    HostFn12,
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
