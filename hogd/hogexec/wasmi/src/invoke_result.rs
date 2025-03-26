use crate::ffi_enums::InvokeErrorCode;

#[repr(C)]
pub struct WasmiInvokeResult {
    result: i32,
    error: i32, // InvokeErrorCode
}

impl WasmiInvokeResult {
    pub fn error(err: InvokeErrorCode) -> WasmiInvokeResult {
        Self {
            result: 0,
            error: err as i32,
        }
    }

    pub fn success(res: i32) -> WasmiInvokeResult {
        Self {
            result: res,
            error: InvokeErrorCode::kNone as i32,
        }
    }
}
