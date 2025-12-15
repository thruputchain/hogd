use crate::debug_log;
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

    pub fn from_wasmtime_trap_code(
        trap: &wasmtime::Trap,
    ) -> WasmtimeInvokeResult {
        crate::debug_log!(eprintln!("Wasmtime trap: {trap:?}"));
        match trap {
            // Gas/fuel related - expected during normal operation
            wasmtime::Trap::OutOfFuel => {
                debug_log!(eprintln!("Wasmtime trap: Out of fuel"));
                Self::error(InvokeErrorCode::kInvokeOutOfGas)
            }

            // Deterministic errors - these are expected from bad contract code
            wasmtime::Trap::MemoryOutOfBounds => {
                debug_log!(eprintln!("Wasmtime trap: Memory out of bounds"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::TableOutOfBounds => {
                debug_log!(eprintln!("Wasmtime trap: Table out of bounds"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::IndirectCallToNull => {
                debug_log!(eprintln!("Wasmtime trap: Indirect call to null"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::BadSignature => {
                debug_log!(eprintln!("Wasmtime trap: Bad function signature"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::IntegerOverflow => {
                debug_log!(eprintln!("Wasmtime trap: Integer overflow"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::IntegerDivisionByZero => {
                debug_log!(eprintln!(
                    "Wasmtime trap: Integer division by zero"
                ));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::BadConversionToInteger => {
                debug_log!(eprintln!(
                    "Wasmtime trap: Bad conversion to integer"
                ));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::UnreachableCodeReached => {
                debug_log!(eprintln!(
                    "Wasmtime trap: Unreachable code reached"
                ));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::NullReference => {
                debug_log!(eprintln!("Wasmtime trap: Null reference"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::ArrayOutOfBounds => {
                debug_log!(eprintln!("Wasmtime trap: Array out of bounds"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::CastFailure => {
                debug_log!(eprintln!("Wasmtime trap: Cast failure"));
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }

            // Errors that should not occur during normal groundhog operation
            // These indicate serious system issues or bugs
            wasmtime::Trap::StackOverflow => {
                debug_log!(eprintln!(
                    "Unrecoverable: Unexpected Wasmtime trap: Stack overflow"
                ));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::HeapMisaligned => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Heap misaligned (atomic op?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::AtomicWaitNonSharedMemory => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Atomic wait on non-shared memory"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::AllocationTooLarge => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Allocation too large"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::Interrupt => {
                debug_log!(eprintln!(
                    "Unrecoverable: Unexpected Wasmtime trap: Interrupt"
                ));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }

            // Component model related (which we should have off)
            wasmtime::Trap::AlwaysTrapAdapter => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Always trap adapter (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::CannotEnterComponent => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot enter component (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::CannotLeaveComponent => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot leave component (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }

            // Async-related (should not occur in our synchronous execution model)
            wasmtime::Trap::NoAsyncResult => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: No async result (async?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::UnhandledTag => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Unhandled tag (async?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::ContinuationAlreadyConsumed => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Continuation already consumed (async?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::AsyncDeadlock => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Async deadlock (async?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }

            // Disabled opcodes (should not occur if validation is correct)
            wasmtime::Trap::DisabledOpcode => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Disabled opcode (pulley?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }

            // Catch-all for any future trap variants (enum is non-exhaustive)
            _ => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Unknown trap: {trap:?}"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
        }
    }
}
