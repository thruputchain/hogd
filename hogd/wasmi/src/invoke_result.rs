// Everything but UnrecoverableSystemError
// is a deterministic error that can be handled by a smart
// contract (i.e. in a try-catch handler).
pub enum WasmiInvokeError {
    None = 0,
    FuncNExist = 1,             // invoked function does not exist
    InputError = 2,             // input validation fails
    ReturnTypeError = 3,        // invoked function did not return expected type
    DeterministicCallError = 4, // some other error, i.e. OOB access in WASM
    OutOfGasError = 5,          // out of gas
    UnrecoverableSystemError = 6,
}

#[repr(C)]
pub struct WasmiInvokeResult {
    result: u64,
    error: u32, // WasmiInvokeError
}

impl WasmiInvokeResult {
    pub fn error(err: WasmiInvokeError) -> WasmiInvokeResult {
        Self {
            result: 0,
            error: err as u32,
        }
    }

    pub fn success(res: u64) -> WasmiInvokeResult {
        Self {
            result: res,
            error: WasmiInvokeError::None as u32,
        }
    }
}
