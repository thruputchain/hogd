use core::ffi::c_void;
use std::slice;
use wasmtime::{Instance, Store};

use crate::context::WasmtimeContext;
use crate::error::TrampolineError;
use crate::invoke_result::WasmtimeInvokeResult;
use common::{InvokeErrorCode, SystemErrorCode};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmtimeModuleStats {
    pub serialized_module_size: usize,
}

impl WasmtimeModuleStats {
    pub fn new() -> Self {
        Self {
            serialized_module_size: 0,
        }
    }
}

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
// the actual EngineInner struct), so it's safe for a WasmtimeRuntime object
// to outlive the parent WasmtimeContext object (although we will probably
// not take advantage of this)
pub struct WasmtimeRuntime {
    pub store: Store<*mut c_void>,
    pub instance: Instance,
}

use crate::debug_log;

impl WasmtimeRuntime {
    // Invariant required: the input wasm module must have passed validation.
    fn new(
        hash: &[u8; 32],
        context: &mut WasmtimeContext,
        userctx: *mut c_void,
    ) -> Option<WasmtimeRuntime> {
        let instance_pre = context.get_instance_pre(hash, userctx).unwrap()?;
        let mut store = Store::new(context.engine(), userctx);
        let instance = match instance_pre.instantiate(&mut store) {
            Ok(instance) => instance,
            Err(e) => {
                if let Some(trap_code) = e.downcast_ref::<wasmtime::Trap>() {
                    if *trap_code == wasmtime::Trap::OutOfFuel {
                        return None;
                    }
                }
                eprintln!("Error instantiating store: {e:?}");
                panic!("Error instantiating store: {e:?}");
            }
        };
        Some(Self { store, instance })
    }

    fn invoke(&mut self, method: &str) -> WasmtimeInvokeResult {
        let func = match self.instance.get_func(&mut self.store, method) {
            Some(v) => v,
            _ => {
                debug_log!(eprintln!("Function {method} not found"));
                return WasmtimeInvokeResult::error(
                    InvokeErrorCode::kInvokeDeterministic,
                );
            }
        };

        match func.call(&mut self.store, &[], &mut []) {
            Ok(_) => WasmtimeInvokeResult::success(),
            Err(err) => {
                if let Some(trap_code) = err.downcast_ref::<wasmtime::Trap>() {
                    return WasmtimeInvokeResult::from_wasmtime_trap_code(
                        trap_code,
                    );
                }
                err.downcast_ref::<TrampolineError>().map_or_else(
                    || {
                        eprintln!(
                            "Unrecoverable: Unknown error from func.call: {err:?}"
                        );
                        WasmtimeInvokeResult::error(
                            InvokeErrorCode::kInvokeUnrecoverable,
                        )
                    },
                    |trampoline_error| {
                        trampoline_error.to_wasmtime_invoke_result()
                    },
                )
            }
        }
    }

    fn get_stats(&self) -> Result<WasmtimeModuleStats, wasmtime::Error> {
        let module = self.instance.module(&self.store);
        let serialized = module.serialize()?;
        Ok(WasmtimeModuleStats {
            serialized_module_size: serialized.len(),
        })
    }
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmtime_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_invoke(
    runtime_void: *mut c_void,
    method_name: *const u8,
    method_name_len: u32,
) -> WasmtimeInvokeResult {
    let runtime: *mut WasmtimeRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    debug_assert!(!runtime.is_null());

    if method_name.is_null() {
        return WasmtimeInvokeResult::error(
            InvokeErrorCode::kInvokeDeterministic,
        );
    }

    let method_name_slice =
        unsafe { slice::from_raw_parts(method_name, method_name_len as usize) };

    let string = match std::str::from_utf8(method_name_slice) {
        Ok(v) => v,
        _ => {
            return WasmtimeInvokeResult::error(
                InvokeErrorCode::kInvokeDeterministic,
            )
        }
    };

    let r = unsafe { &mut *runtime };

    r.invoke(string)
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NewWasmtimeRuntimeResult {
    err_code: i32, // SystemErrorCode
    runtime_pointer: *mut c_void,
}

impl NewWasmtimeRuntimeResult {
    pub fn err(err_code: SystemErrorCode) -> Self {
        Self {
            err_code: err_code as i32,
            runtime_pointer: core::ptr::null_mut(),
        }
    }

    pub fn ok(runtime_pointer: *mut c_void) -> Self {
        Self {
            err_code: 0i32,
            runtime_pointer,
        }
    }

    pub fn get(&self) -> Option<*mut c_void> {
        if self.err_code != 0 {
            return None;
        }
        Some(self.runtime_pointer)
    }
}

/// # Safety
/// wasmtime_context_ptr must not be a nullptr, and must have been created by new_wasmtime_context.
/// userctx can be any (null or non-null) pointer.
/// hash must be a valid pointer to 32 bytes.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_runtime(
    hash: *const u8,      // 32 bytes
    userctx: *mut c_void, // *HostCallContext
    do_verify_contract: bool,
    wasmtime_context_ptr: *mut c_void,
) -> NewWasmtimeRuntimeResult {
    if hash.is_null() || wasmtime_context_ptr.is_null() {
        println!("hash or wasmtime_context_ptr is null");
        return NewWasmtimeRuntimeResult::err(
            SystemErrorCode::kSystemUnrecoverable,
        );
    }

    let hash_slice: &[u8; 32] = unsafe { &*(hash as *const [u8; 32]) };

    let wasmtime_context: &mut WasmtimeContext = unsafe {
        &mut *core::mem::transmute::<*mut c_void, *mut WasmtimeContext>(
            wasmtime_context_ptr,
        )
    };

    if do_verify_contract {
        let slice = match wasmtime_context.fetch_contract(userctx, hash_slice) {
            Ok(Some(slice)) => slice,
            Ok(None) => {
                return NewWasmtimeRuntimeResult::ok(core::ptr::null_mut());
            }
            Err(code) => {
                debug_log!(eprintln!("Error fetching contract: {:?}", code));
                return NewWasmtimeRuntimeResult::err(code);
            }
        };

        if !wasmtime_context.verify_contract(slice) {
            debug_log!(eprintln!("Contract verification failed"));
            return NewWasmtimeRuntimeResult::ok(core::ptr::null_mut());
        }
    }

    let b = match WasmtimeRuntime::new(hash_slice, wasmtime_context, userctx) {
        Some(res) => Box::new(res),
        None => {
            debug_log!(eprintln!("Error creating runtime"));
            return NewWasmtimeRuntimeResult::ok(core::ptr::null_mut());
        }
    };

    NewWasmtimeRuntimeResult::ok(unsafe {
        core::mem::transmute(Box::into_raw(b))
    })
}

/// # Safety
/// Caller must have exclusive (non-concurrent)access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmtime_runtime.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_runtime(p: *mut c_void) {
    let runtime: *mut WasmtimeRuntime = unsafe { core::mem::transmute(p) };
    debug_assert!(!runtime.is_null());

    unsafe { drop(Box::from_raw(runtime)) };
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmtime_runtime.
/// stats_out must be a valid pointer to a WasmtimeModuleStats struct.
/// Returns true on success, false on failure (e.g., serialization error).
#[no_mangle]
pub unsafe extern "C" fn wasmtime_get_module_stats(
    runtime_void: *const c_void,
    stats_out: *mut WasmtimeModuleStats,
) -> bool {
    debug_assert!(!runtime_void.is_null());
    debug_assert!(!stats_out.is_null());

    let runtime: *const WasmtimeRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &*runtime };

    match r.get_stats() {
        Ok(stats) => {
            unsafe {
                *stats_out = stats;
            }
            true
        }
        Err(_) => {
            // On error, set stats to zero values
            unsafe {
                *stats_out = WasmtimeModuleStats::new();
            }
            false
        }
    }
}
