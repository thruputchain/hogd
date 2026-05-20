use crate::wasmi_debug_log as debug_log;
use common::InvokeErrorCode;

#[repr(C)]
pub struct WasmiInvokeResult {
    error: i32, // InvokeErrorCode
    gas_consumed: u64,
}

impl WasmiInvokeResult {
    pub fn error(err: InvokeErrorCode, gas_consumed: u64) -> WasmiInvokeResult {
        Self {
            error: err.0,
            gas_consumed,
        }
    }

    pub fn success(gas_consumed: u64) -> WasmiInvokeResult {
        Self {
            error: InvokeErrorCode::None.0,
            gas_consumed,
        }
    }

    pub fn from_wasmi_trap_code(
        code: wasmi::core::TrapCode,
        gas_consumed: u64,
    ) -> WasmiInvokeResult {
        // all of the wasmi trap codes, in version 0.36,
        // are raised deterministically.
        // For now, at least, we handle all as deterministic errors.
        debug_log!(eprintln!("Wasmi trap code: {code:?}"));
        match code {
            wasmi::core::TrapCode::OutOfFuel => {
                Self::error(InvokeErrorCode::OutOfGas, gas_consumed)
            }
            _ => Self::error(InvokeErrorCode::Deterministic, gas_consumed),
        }
    }
}
