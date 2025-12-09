use common::InvokeErrorCode;

#[repr(C)]
pub struct WasmtimeInvokeResult {
    error: i32, // InvokeErrorCode
}

impl WasmtimeInvokeResult {
    pub fn error(err: InvokeErrorCode) -> WasmtimeInvokeResult {
        Self { error: err as i32 }
    }

    pub fn success() -> WasmtimeInvokeResult {
        Self {
            error: InvokeErrorCode::kInvokeNone as i32,
        }
    }
}
