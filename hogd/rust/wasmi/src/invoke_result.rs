use crate::debug_log;
use common::InvokeErrorCode;
use wasmi::TrapCode;

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

    pub fn from_wasmi_trap_code(
        code: TrapCode,
    ) -> WasmiInvokeResult {
        // all of the wasmi trap codes, in version 0.36,
        // are raised deterministically.
        // For now, at least, we handle all as deterministic errors.
        debug_log!(eprintln!("Wasmi trap code: {code:?}"));
        match code {
            TrapCode::OutOfFuel => {
                Self::error(InvokeErrorCode::kInvokeOutOfGas)
            }
            _ => Self::error(InvokeErrorCode::kInvokeDeterministic),
        }
    }
}
