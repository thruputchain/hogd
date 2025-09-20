use core::ffi::c_void;
use std::slice;
use wasmi::{Instance, Module, Store};

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

use crate::debug_log;

fn handle_trap_code(code: wasmi::core::TrapCode) -> WasmiInvokeResult {
    // all of the wasmi trap codes, in version 0.36,
    // are raised deterministically.
    // For now, at least, we handle all as deterministic errors.
    match code {
        wasmi::core::TrapCode::OutOfFuel => {
            WasmiInvokeResult::error(InvokeErrorCode::kInvokeOutOfGas)
        }
        _ => WasmiInvokeResult::error(InvokeErrorCode::kInvokeDeterministic),
    }
}

// assert that the memory is exported and starts at at most 1 page
fn check_memory_limit(module: &Module) -> bool {
    let export = module.get_export("memory");
    if export.is_none() {
        debug_log!(eprintln!("\"memory\" not exported from module"));
        return false;
    }
    let export_unwrapped = export.unwrap();

    if let wasmi::ExternType::Memory(memory_type) = export_unwrapped {
        debug_log!(if memory_type.initial_pages() > 1.into() {
            eprintln!("Wasm initial memory exceeds limit: {memory_type:?}")
        });
        return memory_type.initial_pages() <= 1.into();
    }
    debug_log!(eprintln!("\"memory\" not exported as wasm memory"));
    false
}

impl WasmiRuntime {
    // Invariant required: the input wasm module must have passed validation.
    fn new(
        bytes: &[u8],
        hash: &[u8; 32],
        context: &mut WasmiContext,
        userctx: *mut c_void,
    ) -> Option<WasmiRuntime> {
        let (module, linker) = if context.has_module_cache() {
            if let Some(module) = context.get_cached_module(hash) {
                (module.module, module.linker)
            } else {
                let linker = context.linker();

                let module =
                    unsafe { Module::new_unchecked(linker.engine(), bytes) }
                        .inspect_err(|_e| {
                            debug_log!(eprintln!(
                                "Module new_unchecked error: {_e}"
                            ))
                        })
                        .ok()?;

                if !check_memory_limit(&module) {
                    debug_log!(eprintln!("Module instantiation failed: Memory export verification failed"));
                    return None;
                }
                context.put_cached_module(hash, &module, &linker);
                (module, linker)
            }
        } else {
            let linker = context.linker();
            let module =
                unsafe { Module::new_unchecked(linker.engine(), bytes) }
                    .inspect_err(|_e| {
                        debug_log!(eprintln!(
                            "Module new_unchecked error: {_e}"
                        ))
                    })
                    .ok()?;

            if !check_memory_limit(&module) {
                debug_log!(eprintln!("Module instantiation failed: Memory export verification failed"));
                return None;
            }
            (module, linker)
        };

        let mut store = Store::new(module.engine(), userctx);
        let instance_result = linker
            .instantiate(&mut store, &module)
            .inspect_err(|_e| {
                debug_log!(eprintln!("Error instantiating store: {_e}"))
            })
            .ok()?;

        let instance = instance_result
            .ensure_no_start(&mut store)
            .inspect_err(|_e| {
                debug_log!(eprintln!("Error ensuring no start: {_e}"))
            })
            .ok()?;
        Some(Self { store, instance })
    }

    fn invoke(&mut self, method: &str) -> WasmiInvokeResult {
        let func = match self.instance.get_func(&self.store, method) {
            Some(v) => v,
            _ => {
                debug_log!(eprintln!("Function {method} not found"));
                return WasmiInvokeResult::error(
                    InvokeErrorCode::kInvokeDeterministic,
                );
            }
        };

        match func.call(&mut self.store, &[], &mut []) {
            Ok(_) => WasmiInvokeResult::success(),
            Err(err) => {
                if let Some(trap_code) = err.as_trap_code() {
                    return handle_trap_code(trap_code);
                }
                err.downcast_ref::<external_call::TrampolineError>()
                    .map_or_else(
                        || {
                            debug_log!(eprintln!(
                                "Unknown error from func.call: {err:?}"
                            ));
                            WasmiInvokeResult::error(
                                InvokeErrorCode::kInvokeUnrecoverable,
                            )
                        },
                        |trampoline_error| {
                            trampoline_error.to_wasmi_invoke_result()
                        },
                    )
            }
        }
    }
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmi_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmi_invoke(
    runtime_void: *mut c_void,
    method_name: *const u8,
    method_name_len: u32,
) -> WasmiInvokeResult {
    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    debug_assert!(!runtime.is_null());

    if method_name.is_null() {
        return WasmiInvokeResult::error(InvokeErrorCode::kInvokeDeterministic);
    }

    let method_name_slice =
        unsafe { slice::from_raw_parts(method_name, method_name_len as usize) };

    let string = match std::str::from_utf8(method_name_slice) {
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

/// # Safety
/// wasmi_context_ptr must not be a nullptr, and must have been created by new_wasmi_context.
/// userctx can be any (null or non-null) pointer.
/// bytes must be either null or a valid memory slice.
#[no_mangle]
pub unsafe extern "C" fn new_wasmi_runtime(
    bytes: *const u8,
    bytes_len: u32,
    hash: *const u8, // 32 bytes
    userctx: *mut c_void,
    wasmi_context_ptr: *mut c_void,
) -> *mut c_void {
    if bytes.is_null() {
        return core::ptr::null_mut();
    }
    debug_assert!(!hash.is_null());
    debug_assert!(!wasmi_context_ptr.is_null());

    let hash_slice: &[u8; 32] = unsafe { &*(hash as *const [u8; 32]) };

    let wasmi_context: &mut WasmiContext = unsafe {
        &mut *core::mem::transmute::<*mut c_void, *mut WasmiContext>(
            wasmi_context_ptr,
        )
    };

    let slice = unsafe { slice::from_raw_parts(bytes, bytes_len as usize) };

    let b = match WasmiRuntime::new(slice, hash_slice, wasmi_context, userctx) {
        Some(res) => Box::new(res),
        None => {
            return core::ptr::null_mut();
        }
    };

    unsafe { core::mem::transmute(Box::into_raw(b)) }
}

/// # Safety
/// Caller must have exclusive (non-concurrent)access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmi_runtime.
#[no_mangle]
pub unsafe extern "C" fn free_wasmi_runtime(p: *mut c_void) {
    let runtime: *mut WasmiRuntime = unsafe { core::mem::transmute(p) };
    debug_assert!(!runtime.is_null());

    unsafe { drop(Box::from_raw(runtime)) };
}
