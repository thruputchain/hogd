pub enum WasmiInvokeError {
    None = 0,
    LinkError = 1,
    FuncNExist = 2,
    InputError = 3, // input validation fails
    ReturnTypeError = 4,
    DeterministicCallError = 5,
    OutOfGasError = 6,
    UnrecoverableSystemError = 7,
}

#[repr(C)]
pub struct WasmiInvokeResult {
    result: u64,
    error: u32,
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
