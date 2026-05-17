use core::ffi::c_void;

use crate::runtime::WasmtimeRuntime;

pub mod context;
pub mod error;
pub mod global_context;
pub mod instance_pre_cache;
pub mod invoke_result;
pub mod runtime;

type SHA256Hash = [u8; 32];

// Re-export FFI functions and types for testing and external use
pub use context::{
    free_wasmtime_context, new_wasmtime_context, wasmtime_verify_contract,
    ContractGetFn, ContractGetResult,
};
pub use global_context::{
    free_wasmtime_global_context, new_wasmtime_global_context,
    wasmtime_global_link_nargs,
};
pub use instance_pre_cache::InstancePreCache;

pub use invoke_result::WasmtimeInvokeResult;
pub use runtime::{
    free_wasmtime_runtime, new_wasmtime_runtime, wasmtime_get_module_stats,
    wasmtime_invoke, WasmtimeModuleStats,
};

#[cfg(feature = "debug_logging")]
#[macro_export]
macro_rules! debug_log {
    ($e:expr) => {
        $e
    };
}

#[cfg(not(feature = "debug_logging"))]
#[macro_export]
macro_rules! debug_log {
    ($e:expr) => {
        ()
    };
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmtime_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_get_available_gas(
    runtime_void: *const c_void,
) -> u64 {
    debug_assert!(!runtime_void.is_null());
    let runtime: *const WasmtimeRuntime =
        runtime_void.cast::<WasmtimeRuntime>();
    let r = unsafe { &*runtime };
    match r.store.get_fuel() {
        Ok(fuel) => fuel,
        Err(_) => {
            eprintln!("FATAL: get_fuel failed");
            std::process::abort();
        }
    }
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmtime_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_set_available_gas(
    runtime_void: *mut c_void,
    gas: u64,
) {
    debug_assert!(!runtime_void.is_null());
    let runtime: *mut WasmtimeRuntime = runtime_void.cast::<WasmtimeRuntime>();
    let r = unsafe { &mut *runtime };
    if r.store.set_fuel(gas).is_err() {
        eprintln!("FATAL: set_fuel failed");
        std::process::abort();
    }
}
