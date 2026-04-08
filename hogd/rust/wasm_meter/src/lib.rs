use core::slice;
use std::ptr;

#[repr(C)]
pub struct WasmMeterInstrumentResult {
    pub data: *mut u8,
    pub len: usize,
    pub success: bool,
}

/// # Safety
/// `wasm_ptr` must point to `wasm_len` bytes of valid wasm bytecode.
#[no_mangle]
pub unsafe extern "C" fn wasm_meter_instrument(
    wasm_ptr: *const u8,
    wasm_len: usize,
) -> WasmMeterInstrumentResult {
    if wasm_ptr.is_null() {
        return WasmMeterInstrumentResult {
            data: ptr::null_mut(),
            len: 0,
            success: false,
        };
    }
    let wasm = unsafe { slice::from_raw_parts(wasm_ptr, wasm_len) };
    match groundhog_wasm_meter::instrument(wasm) {
        Ok(bytes) => {
            let mut boxed = bytes.into_boxed_slice();
            let data = boxed.as_mut_ptr();
            let len = boxed.len();
            std::mem::forget(boxed);
            WasmMeterInstrumentResult {
                data,
                len,
                success: true,
            }
        }
        Err(e) => {
            eprintln!("wasm_meter_instrument failed: {e}");
            WasmMeterInstrumentResult {
                data: ptr::null_mut(),
                len: 0,
                success: false,
            }
        }
    }
}

/// # Safety
/// `ptr` and `len` must have been returned by a successful `wasm_meter_instrument` call.
/// Must not be called more than once for the same allocation.
#[no_mangle]
pub unsafe extern "C" fn wasm_meter_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        unsafe {
            drop(Box::from_raw(slice::from_raw_parts_mut(ptr, len)));
        }
    }
}
