use crate::ffi_enums::InvokeErrorCode;

#[repr(C)]
pub struct WasmiInvokeResult {
    error: i32, // InvokeErrorCode
}

impl WasmiInvokeResult {
    pub fn error(err: InvokeErrorCode) -> WasmiInvokeResult {
        Self { error: err as i32 }
    }

    pub fn success() -> WasmiInvokeResult {
        Self {
            error: InvokeErrorCode::kInvokeNone as i32,
        }
    }
}
