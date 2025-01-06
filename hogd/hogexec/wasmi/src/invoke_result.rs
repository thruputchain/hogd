// Everything but UnrecoverableSystemError
// is a deterministic error that can be handled by a smart
// contract (i.e. in a try-catch handler).
#[repr(i32)]
pub enum InvokeError {
    None = 0,
    Deterministic = 1,
    OutOfGas = 2,
    Unrecoverable = 3,
}

#[repr(C)]
pub struct WasmiInvokeResult {
    result: i32,
    error: i32, // InvokeError
}

impl WasmiInvokeResult {
    pub fn error(err: InvokeError) -> WasmiInvokeResult {
        Self {
            result: 0,
            error: err as i32,
        }
    }

    pub fn success(res: i32) -> WasmiInvokeResult {
        Self {
            result: res,
            error: InvokeError::None as i32,
        }
    }
}
