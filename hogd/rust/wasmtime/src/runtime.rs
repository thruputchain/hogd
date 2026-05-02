use core::ffi::c_void;
use std::slice;
use wasmtime::{Instance, Store};

use crate::context::WasmtimeContext;
use crate::error::TrampolineError;
use crate::invoke_result::{
    wasmtime_trap_to_invoke_error, WasmtimeInvokeResult,
};
use common::{InvokeErrorCode, SystemErrorCode};

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct WasmtimeModuleStats {
    pub serialized_module_size: usize,
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
    // userctx must be a valid pointer to a HostCallContext
    //
    // Returns:
    //   Ok(Some(runtime)) — success
    //   Ok(None) — deterministic failure (e.g. trap during instantiation, OOB data segment)
    //   Err(code) — nondeterministic system error (e.g. pool exhaustion, OOM)
    fn new(
        hash: &[u8; 32],
        context: &mut WasmtimeContext,
        userctx: *mut c_void,
    ) -> Result<Option<WasmtimeRuntime>, SystemErrorCode> {
        let instance_pre =
            match unsafe { context.get_instance_pre(hash, userctx) } {
                Ok(Some(pre)) => pre,
                Ok(None) => return Ok(None),
                Err(code) => return Err(code),
            };
        // Each WasmtimeRuntime gets a fresh Store. As of Wasmtime 42.0.0 this
        // means cross-contract nested invokes recompute stack_limit from the
        // current SP instead of inheriting a caller limit from another Store.
        // Same-Store recursive reentry is the case that reuses an existing
        // limit in sync mode.
        // References:
        // - wasmtime-42.0.0/src/runtime/func.rs:1469-1482
        // - wasmtime-42.0.0/src/runtime/func.rs:1518-1527
        // - wasmtime-42.0.0/src/runtime/vm/vmcontext.rs:1258-1264
        let mut store = Store::new(context.engine(), userctx);
        let instance = match instance_pre.instantiate(&mut store) {
            Ok(instance) => instance,
            Err(e) => {
                if let Some(trap) = e.downcast_ref::<wasmtime::Trap>() {
                    match wasmtime_trap_to_invoke_error(trap) {
                        // Deterministic trap during instantiation (e.g. OOB
                        // data segment, start function trap including
                        // OutOfFuel).
                        InvokeErrorCode::OutOfGas
                        | InvokeErrorCode::Deterministic => {
                            debug_log!(eprintln!(
                                "Trap during instantiation: {e:?}"
                            ));
                            return Ok(None);
                        }
                        other => {
                            panic!(
                                "Unrecoverable: nondeterministic trap during instantiation: {trap:?}, classified as {other:?}"
                            );
                        }
                    }
                }
                // Non-trap error (e.g. pooling allocator exhaustion, OOM).
                // This is nondeterministic — the module itself may be fine.
                debug_log!(eprintln!(
                    "System error during instantiation: {e:?}"
                ));
                return Err(SystemErrorCode::Unrecoverable);
            }
        };
        let stack_limit_global = match instance
            .get_global(&mut store, "__groundhog_stack_limit")
        {
            Some(g) => g,
            None => {
                eprintln!("Unrecoverable: instrumented module missing __groundhog_stack_limit");
                return Err(SystemErrorCode::Unrecoverable);
            }
        };
        // 4096 = 1/4 of max_wasm_stack (16KB) configured in global_context.rs
        if stack_limit_global
            .set(&mut store, wasmtime::Val::I64(4096))
            .is_err()
        {
            eprintln!("Unrecoverable: failed to set __groundhog_stack_limit");
            return Err(SystemErrorCode::Unrecoverable);
        }
        Ok(Some(Self { store, instance }))
    }

    fn invoke(&mut self, method: &str) -> WasmtimeInvokeResult {
        let func = match self.instance.get_func(&mut self.store, method) {
            Some(v) => v,
            _ => {
                debug_log!(eprintln!("Function {method} not found"));
                return WasmtimeInvokeResult::error(
                    InvokeErrorCode::Deterministic,
                );
            }
        };

        let func_type = func.ty(&self.store);
        if func_type.params().len() != 0 || func_type.results().len() != 0 {
            debug_log!(eprintln!(
                "Function {method} has unsupported ABI: expected () -> (), got {func_type:?}"
            ));
            return WasmtimeInvokeResult::error(InvokeErrorCode::Deterministic);
        }

        match func.call(&mut self.store, &[], &mut []) {
            Ok(_) => WasmtimeInvokeResult::success(),
            Err(err) => {
                if let Some(trap_code) = err.downcast_ref::<wasmtime::Trap>() {
                    #[cfg(feature = "wasm_debug")]
                    {
                        eprintln!("WASM backtrace:\n{:#}", err);
                    }
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
                            InvokeErrorCode::Unrecoverable,
                        )
                    },
                    |trampoline_error| {
                        #[cfg(feature = "wasm_debug")]
                        match trampoline_error.error {
                            // We don't print the backtrace for "errors" that correspond to normal shutdown
                            common::HostFnErrorCode::kHostReturn => {},
                            _ => {
                                eprintln!("WASM backtrace:\n{:#}", err);
                            }
                        }
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
    let runtime: *mut WasmtimeRuntime = runtime_void.cast::<WasmtimeRuntime>();
    debug_assert!(!runtime.is_null());

    if method_name.is_null() {
        return WasmtimeInvokeResult::error(InvokeErrorCode::Deterministic);
    }

    let method_name_slice =
        unsafe { slice::from_raw_parts(method_name, method_name_len as usize) };

    let string = match std::str::from_utf8(method_name_slice) {
        Ok(v) => v,
        _ => {
            return WasmtimeInvokeResult::error(InvokeErrorCode::Deterministic)
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
            err_code: err_code.0,
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

    pub fn err_code(&self) -> i32 {
        self.err_code
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
    wasmtime_context_ptr: *mut c_void,
) -> NewWasmtimeRuntimeResult {
    if hash.is_null() || wasmtime_context_ptr.is_null() {
        eprintln!("hash or wasmtime_context_ptr is null");
        return NewWasmtimeRuntimeResult::err(SystemErrorCode::Unrecoverable);
    }

    let hash_slice: &[u8; 32] = unsafe { &*(hash as *const [u8; 32]) };

    let wasmtime_context: &mut WasmtimeContext =
        unsafe { &mut *wasmtime_context_ptr.cast::<WasmtimeContext>() };

    let b = match WasmtimeRuntime::new(hash_slice, wasmtime_context, userctx) {
        Ok(Some(res)) => Box::new(res),
        Ok(None) => {
            debug_log!(eprintln!("Error creating runtime"));
            return NewWasmtimeRuntimeResult::ok(core::ptr::null_mut());
        }
        Err(code) => {
            debug_log!(eprintln!("System error creating runtime: {:?}", code));
            return NewWasmtimeRuntimeResult::err(code);
        }
    };

    NewWasmtimeRuntimeResult::ok(Box::into_raw(b).cast::<c_void>())
}

/// # Safety
/// If p is null, this function is a no-op.
/// If p is non-null, caller must have exclusive (non-concurrent) access,
/// p must have been created by new_wasmtime_runtime, and runtime must not be
/// accessed after this call.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_runtime(p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let runtime: *mut WasmtimeRuntime = p.cast::<WasmtimeRuntime>();
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
        runtime_void.cast::<WasmtimeRuntime>();
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
                *stats_out = WasmtimeModuleStats::default();
            }
            false
        }
    }
}
