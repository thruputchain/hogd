use std::slice;

use crate::ffi_enums::InvokeErrorCode;
use crate::invoke_result::WasmiInvokeResult;

pub fn str_from_c_str(
    data: &*const u8,
    len: u32,
) -> Result<&str, WasmiInvokeResult> {
    if *data == core::ptr::null() {
        return Err(WasmiInvokeResult::error(InvokeErrorCode::kDeterministic));
    }

    let slice: &[u8] = unsafe { slice::from_raw_parts(*data, len as usize) };

    match std::str::from_utf8(slice) {
        Ok(v) => Ok(v),
        _ => Err(WasmiInvokeResult::error(InvokeErrorCode::kDeterministic)),
    }
}
