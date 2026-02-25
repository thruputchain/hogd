use crate::debug_log;
use common::InvokeErrorCode;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct WasmtimeInvokeResult {
    pub error: i32, // InvokeErrorCode
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
                Self::error(InvokeErrorCode::kInvokeOutOfGas)
            }

            // Deterministic errors - these are expected from bad contract code
            wasmtime::Trap::MemoryOutOfBounds => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::TableOutOfBounds => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::IndirectCallToNull => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::BadSignature => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::IntegerOverflow => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::IntegerDivisionByZero => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::BadConversionToInteger => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::UnreachableCodeReached => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::NullReference => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::ArrayOutOfBounds => {
                Self::error(InvokeErrorCode::kInvokeDeterministic)
            }
            wasmtime::Trap::CastFailure => {
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
            wasmtime::Trap::CannotEnterComponent => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot enter component (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::CannotLeaveComponent => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot leave component (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::CannotBlockSyncTask => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot block sync task (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::InvalidChar => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Invalid char (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::StringOutOfBounds => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: String out of bounds (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::ListOutOfBounds => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: List out of bounds (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::InvalidDiscriminant => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Invalid discriminant (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::UnalignedPointer => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Unaligned pointer (component model?)"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }

            // Debug assertions (should only fire in debug builds)
            wasmtime::Trap::DebugAssertStringEncodingFinished => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Debug assert string encoding finished"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::DebugAssertEqualCodeUnits => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Debug assert equal code units"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::DebugAssertPointerAligned => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Debug assert pointer aligned"));
                Self::error(InvokeErrorCode::kInvokeUnrecoverable)
            }
            wasmtime::Trap::DebugAssertUpperBitsUnset => {
                debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Debug assert upper bits unset"));
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
