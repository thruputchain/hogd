use core::ffi::c_void;

pub mod context;
pub mod external_call;
pub mod invoke_result;
pub mod runtime;

use crate::runtime::WasmiRuntime;

#[repr(C)]
pub struct MemorySlice {
    mem: *mut u8,
    sz: u32,
}

#[no_mangle]
pub extern "C" fn wasmi_get_memory(runtime_void: *mut c_void) -> MemorySlice {
    assert!(runtime_void != core::ptr::null_mut());

    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };

    let r = unsafe { &mut *runtime };

    match r.instance.get_memory(&r.store, "memory") {
        Some(mem) => {
            let mem_sz = mem.data_size(&r.store) as u32;
            // wasmi docs say that _any_ call to data_ptr or use of store
            // can invalidate returned pointers
            MemorySlice {
                mem: mem.data_ptr(&r.store),
                sz: mem_sz,
            }
        }
        _ => MemorySlice {
            mem: std::ptr::null_mut(),
            sz: 0,
        },
    }
}

#[no_mangle]
pub extern "C" fn wasmi_consume_gas(
    runtime_void: *mut c_void,
    gas_consumed: u64,
) -> bool {
    assert!(runtime_void != core::ptr::null_mut());

    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };

    let r = unsafe { &mut *runtime };

    let cur_fuel: u64 = r.store.get_fuel().unwrap();
    if cur_fuel > gas_consumed {
        r.store.set_fuel(cur_fuel - gas_consumed).unwrap();
        return true;
    }
    r.store.set_fuel(0).unwrap();
    return false;
}

#[no_mangle]
pub extern "C" fn wasmi_get_available_gas(runtime_void: *const c_void) -> u64 {
    assert!(runtime_void != core::ptr::null());
    let runtime: *const WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &*runtime };
    return r.store.get_fuel().unwrap();
}

#[no_mangle]
pub extern "C" fn wasmi_set_available_gas(runtime_void: *mut c_void, gas: u64) {
    assert!(runtime_void != core::ptr::null_mut());
    let runtime: *mut WasmiRuntime =
        unsafe { core::mem::transmute(runtime_void) };
    let r = unsafe { &mut *runtime };
    r.store.set_fuel(gas).unwrap();
}
