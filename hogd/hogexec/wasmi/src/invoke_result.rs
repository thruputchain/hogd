// Everything but UnrecoverableSystemError
// is a deterministic error that can be handled by a smart
// contract (i.e. in a try-catch handler).
#[repr(u8)]
pub enum InvokeError {
    None = 0,
    DeterministicError = 1,
    OutOfGasError = 2,
    UnrecoverableSystemError = 3,
}

#[repr(C)]
pub struct WasmiInvokeResult {
    result: u64,
    error: u8, // WasmiInvokeError
}

impl WasmiInvokeResult {
    pub fn error(err: InvokeError) -> WasmiInvokeResult {
        Self {
            result: 0,
            error: err as u8,
        }
    }

    pub fn success(res: u64) -> WasmiInvokeResult {
        Self {
            result: res,
            error: InvokeError::None as u8,
        }
    }
}
