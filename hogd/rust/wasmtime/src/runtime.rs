use core::ffi::c_void;
use std::slice;
use wasmtime::{Instance, Module, Store};

use crate::context::WasmtimeContext;
use crate::error::TrampolineError;
use crate::invoke_result::WasmtimeInvokeResult;
use common::InvokeErrorCode;

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

// assert that the memory is exported and starts at at most 1 page
fn check_memory_limit(module: &Module) -> bool {
    let export = module.get_export("memory");
    if export.is_none() {
        debug_log!(eprintln!("\"memory\" not exported from module"));
        return false;
    }
    let export_unwrapped = export.unwrap();

    if let wasmtime::ExternType::Memory(memory_type) = export_unwrapped {
        debug_log!(if memory_type.minimum() > 1u64 {
            eprintln!("Wasm initial memory exceeds limit: {memory_type:?}")
        });
        return memory_type.minimum() <= 1u64;
    }
    debug_log!(eprintln!("\"memory\" not exported as wasm memory"));
    false
}

fn check_no_start(module: &Module) -> bool {
    let export = module.get_export("start");
    export.is_none()
}

impl WasmtimeRuntime {
    // Invariant required: the input wasm module must have passed validation.
    fn new(
        bytes: &[u8],
        hash: &[u8; 32],
        context: &mut WasmtimeContext,
        userctx: *mut c_void,
    ) -> Option<WasmtimeRuntime> {
        let instance_pre = if let Some(module) = context.get_cached_module(hash).unwrap()
        {
            let instance_pre = context
                .linker()
                .instantiate_pre(&module)
                .inspect_err(|_e| eprintln!("Error instantiating store: {_e}"))
                .ok()?;
            instance_pre
        } else {
            let linker = context.linker();
            let module = Module::new(linker.engine(), bytes)
                .inspect_err(|_e| {
                    debug_log!(eprintln!("Module new error: {_e}"))
                })
                .ok()?;

            if !check_memory_limit(&module) {
                debug_log!(eprintln!("Module instantiation failed: Memory export verification failed"));
                return None;
            }
            if !check_no_start(&module) {
                debug_log!(eprintln!(
                    "Module instantiation failed: Has start function"
                ));
                return None;
            }

            let instance_pre = linker
                .instantiate_pre(&module)
                .inspect_err(|_e| eprintln!("Error instantiating store: {_e}"))
                .ok()?;
            context.put_cached_module(hash, &module).unwrap();
            instance_pre
        };

        let mut store = Store::new(context.engine(), userctx);
        let instance = instance_pre
            .instantiate(&mut store)
            .inspect_err(|_e| eprintln!("Error instantiating store: {_e}"))
            .ok()?;
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
) -> *mut c_void {
    if hash.is_null() || wasmtime_context_ptr.is_null() {
        println!("hash or wasmtime_context_ptr is null");
        return core::ptr::null_mut();
    }

    let hash_slice: &[u8; 32] = unsafe { &*(hash as *const [u8; 32]) };

    let wasmtime_context: &mut WasmtimeContext = unsafe {
        &mut *core::mem::transmute::<*mut c_void, *mut WasmtimeContext>(
            wasmtime_context_ptr,
        )
    };

    let slice = match wasmtime_context.fetch_contract(userctx, hash_slice) {
        Ok(Some(slice)) => slice,
        Ok(None) => {
            println!("No contract found");
            return core::ptr::null_mut();
        }
        Err(_code) => {
            println!("Error fetching contract: {:?}", _code);
            return core::ptr::null_mut();
        }
    };

    if do_verify_contract && !wasmtime_context.verify_contract(slice) {
        println!("Contract verification failed");
        return core::ptr::null_mut();
    }

    let b = match WasmtimeRuntime::new(
        slice,
        hash_slice,
        wasmtime_context,
        userctx,
    ) {
        Some(res) => Box::new(res),
        None => {
            println!("Error creating runtime");
            return core::ptr::null_mut();
        }
    };

    unsafe { core::mem::transmute(Box::into_raw(b)) }
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
