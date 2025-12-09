use core::ffi::c_void;

use crate::runtime::WasmtimeRuntime;

pub mod context;
pub mod invoke_result;
pub mod error;
pub mod runtime;





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

#[repr(C)]
pub struct WasmtimeMemorySlice {
    mem: *mut u8,
    sz: u32,
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmtime_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_get_memory(
    runtime_void: *mut c_void,
) -> WasmtimeMemorySlice {
    debug_assert!(!runtime_void.is_null());

    let runtime: *mut WasmtimeRuntime =
        unsafe { core::mem::transmute(runtime_void) };

    let r = unsafe { &mut *runtime };

    match r.instance.get_memory(&r.store, "memory") {
        Some(mem) => {
            let mem_sz = mem.data_size(&r.store) as u32;
            // wasmi docs say that _any_ call to data_ptr or use of store
            // can invalidate returned pointers
            WasmtimeMemorySlice {
                mem: mem.data_ptr(&r.store),
                sz: mem_sz,
            }
        }
        _ => WasmtimeMemorySlice {
            mem: std::ptr::null_mut(),
            sz: 0,
        },
    }
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
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &*runtime };
    r.store.get_fuel().unwrap()
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
    let runtime: *mut WasmtimeRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &mut *runtime };
    r.store.set_fuel(gas).unwrap();
}
