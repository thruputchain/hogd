use core::ffi::c_void;
use std::slice;
use wasmi::{Instance, Module, Store, Val};

use crate::context::WasmiContext;
use crate::external_call;
use crate::ffi_enums::InvokeErrorCode;
use crate::invoke_result::WasmiInvokeResult;

// A WasmiRuntime implements a single WebAssembly module,
// with the ability to invoke exported functions and call
// in to provided syscalls.
//
// Notes:
// Store has an internal ID, for error checking the internals.
// A lot of elements inside are indices to buffers within Store,
// which is obviously broken if the wrong store is used.
// Wasmi guards against this by giving each Store a unique id,
// which is derived from an AtomicU32.fetch_add on a static variable.
// This is fine for our case, as AtomicU32 wraps (does not panic!) on
// overflow.
//
// Store/module/instance store internally an Engine (which is an Arc around
// the actual EngineInner struct), so it's safe for a WasmiRuntime object
// to outlive the parent WasmiContext object (although we will probably
// not take advantage of this)
pub struct WasmiRuntime {
    pub store: Store<*mut c_void>,
    pub instance: Instance,
}

fn assert_runtime_not_null(runtime: *const WasmiRuntime) {
    assert!(runtime != core::ptr::null());
}

fn handle_trap_code(code: wasmi::core::TrapCode) -> WasmiInvokeResult {
    // all of the wasmi trap codes, in version 0.36,
    // are raised deterministically.
    // For now, at least, we handle all as deterministic errors.
    match code {
        wasmi::core::TrapCode::OutOfFuel => {
            return WasmiInvokeResult::error(InvokeErrorCode::kInvokeOutOfGas);
        }
        _ => {
            return WasmiInvokeResult::error(
                InvokeErrorCode::kInvokeDeterministic,
            );
        }
    }
}

impl WasmiRuntime {
    // Invariant required: the input wasm module must have passed validation.
    fn new(
        bytes: &[u8],
        context: &WasmiContext,
        userctx: *mut c_void,
    ) -> Option<WasmiRuntime> {
        let module =
            match unsafe { Module::new_unchecked(&context.engine, &bytes) } {
                Ok(m) => m,
                Err(_) => {
                    return None;
                }
            };

        let mut store = Store::new(&context.engine, userctx);

        let instance = match context.linker.instantiate(&mut store, &module) {
            Ok(inst_pre) => match inst_pre.ensure_no_start(&mut store) {
                Ok(inst) => inst,
                Err(_) => {
                    return None;
                }
            },
            Err(_) => {
                return None;
            }
        };
        Some(Self {
            store: store,
            instance: instance,
        })
    }

    fn invoke(&mut self, method: &str) -> WasmiInvokeResult {
        let func = match self.instance.get_func(&self.store, method) {
            Some(v) => v,
            _ => {
                return WasmiInvokeResult::error(
                    InvokeErrorCode::kInvokeDeterministic,
                );
            }
        };

        let mut res = [Val::I32(0)];

        let func_res = func.call(&mut self.store, &[], &mut res);

        match func_res {
            Ok(_) => match res[0].i32() {
                Some(v) => {
                    return WasmiInvokeResult::success(v as i32);
                }
                _ => {
                    return WasmiInvokeResult::error(
                        InvokeErrorCode::kInvokeDeterministic,
                    );
                }
            },
            Err(err) => {
                match err.as_trap_code() {
                    Some(trap_code) => {
                        return handle_trap_code(trap_code);
                    }
                    _ => {}
                };
                let my_error = match err
                    .downcast_ref::<external_call::TrampolineError>()
                {
                    Some(trampoline_error) => trampoline_error.clone(),
                    None => {
                        return WasmiInvokeResult::error(
                            InvokeErrorCode::kInvokeUnrecoverable,
                        );
                    }
                };

                match my_error.error {
                    external_call::TrampolineErrorEnum::OutOfGas => {
                        return WasmiInvokeResult::error(
                            InvokeErrorCode::kInvokeOutOfGas,
                        );
                    }
                    external_call::TrampolineErrorEnum::Return => {
                        // Return no error code if successful return
                        return WasmiInvokeResult::success(0);
                    }
                    external_call::TrampolineErrorEnum::Exit { code } => {
                        return WasmiInvokeResult::success(code);
                    }
                    external_call::TrampolineErrorEnum::ValidationFailure => {
                        return WasmiInvokeResult::error(
                            InvokeErrorCode::kInvokeValidationFailure,
                        );
                    }
                    _ => {
                        return WasmiInvokeResult::error(
                            InvokeErrorCode::kInvokeUnrecoverable,
                        );
                    }
                }
            }
        };
    }
}

#[no_mangle]
pub extern "C" fn wasmi_invoke(
    runtime_void: *mut c_void,
    method_name: *const u8,
    method_name_len: u32,
) -> WasmiInvokeResult {
    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };

    assert_runtime_not_null(runtime);

    if method_name == core::ptr::null() {
        return WasmiInvokeResult::error(InvokeErrorCode::kInvokeDeterministic);
    }

    let method_name_slice =
        unsafe { slice::from_raw_parts(method_name, method_name_len as usize) };

    let string = match std::str::from_utf8(&method_name_slice) {
        Ok(v) => v,
        _ => {
            return WasmiInvokeResult::error(
                InvokeErrorCode::kInvokeDeterministic,
            )
        }
    };

    let r = unsafe { &mut *runtime };

    r.invoke(string)
}

#[no_mangle]
pub extern "C" fn new_wasmi_runtime(
    bytes: *const u8,
    bytes_len: u32,
    userctx: *mut c_void,
    wasmi_context_ptr: *const c_void,
) -> *mut c_void {
    if bytes == core::ptr::null() {
        return core::ptr::null_mut();
    }

    assert!(wasmi_context_ptr != core::ptr::null());

    let wasmi_context: &WasmiContext = unsafe {
        &*core::mem::transmute::<_, *mut WasmiContext>(wasmi_context_ptr)
    };

    let slice = unsafe { slice::from_raw_parts(bytes, bytes_len as usize) };

    let b = match WasmiRuntime::new(&slice, &wasmi_context, userctx) {
        Some(res) => Box::new(res),
        None => {
            return core::ptr::null_mut();
        }
    };

    return unsafe { core::mem::transmute(Box::into_raw(b)) };
}

#[no_mangle]
pub extern "C" fn free_wasmi_runtime(p: *mut c_void) {
    let runtime: *mut WasmiRuntime = unsafe { core::mem::transmute(p) };

    assert_runtime_not_null(runtime);

    unsafe { drop(Box::from_raw(runtime)) };
}
