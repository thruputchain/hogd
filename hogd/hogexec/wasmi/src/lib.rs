use core::ffi::c_void;

pub mod context;
mod external_call;
#[rustfmt::skip]
pub mod ffi_enums;
pub mod invoke_result;
pub mod module_cache;
pub mod runtime;

use crate::runtime::WasmiRuntime;

#[repr(C)]
pub struct WasmiMemorySlice {
    mem: *mut u8,
    sz: u32,
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmi_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmi_get_memory(
    runtime_void: *mut c_void,
) -> WasmiMemorySlice {
    debug_assert!(!runtime_void.is_null());

    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };

    let r = unsafe { &mut *runtime };

    match r.instance.get_memory(&r.store, "memory") {
        Some(mem) => {
            let mem_sz = mem.data_size(&r.store) as u32;
            // wasmi docs say that _any_ call to data_ptr or use of store
            // can invalidate returned pointers
            WasmiMemorySlice {
                mem: mem.data_ptr(&r.store),
                sz: mem_sz,
            }
        }
        _ => WasmiMemorySlice {
            mem: std::ptr::null_mut(),
            sz: 0,
        },
    }
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the runtime, and runtime must not be a nullptr.
/// Runtime must have been created by new_wasmi_runtime.
#[no_mangle]
pub unsafe extern "C" fn wasmi_get_available_gas(
    runtime_void: *const c_void,
) -> u64 {
    debug_assert!(!runtime_void.is_null());
    let runtime: *const WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &*runtime };
    r.store.get_fuel().unwrap()
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
    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &mut *runtime };
    r.store.set_fuel(gas).unwrap();
}
