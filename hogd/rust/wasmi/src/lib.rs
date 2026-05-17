use core::ffi::c_void;

mod module_cache;
pub mod wasmi_context;
mod wasmi_error;
pub mod wasmi_invoke_result;
pub mod wasmi_runtime;

type SHA256Hash = [u8; 32];

use crate::wasmi_runtime::WasmiRuntime;

pub use common::contract_loader::ContractGetFn;
pub use common::contract_loader::ContractGetResult;

#[cfg(feature = "debug_logging")]
#[macro_export]
macro_rules! wasmi_debug_log {
    ($e:expr) => {
        $e
    };
}

#[cfg(not(feature = "debug_logging"))]
#[macro_export]
macro_rules! wasmi_debug_log {
    ($e:expr) => {
        ()
    };
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmi_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmi_get_available_gas(
    runtime_void: *const c_void,
) -> u64 {
    debug_assert!(!runtime_void.is_null());
    let runtime: *const WasmiRuntime = runtime_void.cast::<WasmiRuntime>();
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
/// Runtime must have been created by new_wasmi_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmi_set_available_gas(
    runtime_void: *mut c_void,
    gas: u64,
) {
    debug_assert!(!runtime_void.is_null());
    let runtime: *mut WasmiRuntime = runtime_void.cast::<WasmiRuntime>();
    let r = unsafe { &mut *runtime };
    if r.store.set_fuel(gas).is_err() {
        eprintln!("FATAL: set_fuel failed");
        std::process::abort();
    }
}
