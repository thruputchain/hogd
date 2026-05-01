use crate::debug_log;
use common::InvokeErrorCode;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct WasmtimeInvokeResult {
    pub error: i32, // InvokeErrorCode
}

impl WasmtimeInvokeResult {
    pub fn error(err: InvokeErrorCode) -> WasmtimeInvokeResult {
        Self { error: err.0 }
    }

    pub fn success() -> WasmtimeInvokeResult {
        Self {
            error: InvokeErrorCode::None.0,
        }
    }

    pub fn from_wasmtime_trap_code(
        trap: &wasmtime::Trap,
    ) -> WasmtimeInvokeResult {
        Self::error(wasmtime_trap_to_invoke_error(trap))
    }
}

pub fn wasmtime_trap_to_invoke_error(trap: &wasmtime::Trap) -> InvokeErrorCode {
    crate::debug_log!(eprintln!("Wasmtime trap: {trap:?}"));
    match trap {
        // Gas/fuel related - expected during normal operation
        wasmtime::Trap::OutOfFuel => InvokeErrorCode::OutOfGas,

        // Deterministic errors - these are expected from bad contract code
        wasmtime::Trap::MemoryOutOfBounds => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::TableOutOfBounds => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::IndirectCallToNull => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::BadSignature => InvokeErrorCode::Deterministic,
        wasmtime::Trap::IntegerOverflow => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::IntegerDivisionByZero => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::BadConversionToInteger => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::UnreachableCodeReached => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::NullReference => InvokeErrorCode::Deterministic,
        wasmtime::Trap::ArrayOutOfBounds => {
            InvokeErrorCode::Deterministic
        }
        wasmtime::Trap::CastFailure => InvokeErrorCode::Deterministic,

        // Errors that should not occur during normal groundhog operation
        // These indicate serious system issues or bugs
        wasmtime::Trap::StackOverflow => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Stack overflow"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::HeapMisaligned => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Heap misaligned (atomic op?)"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::AtomicWaitNonSharedMemory => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Atomic wait on non-shared memory"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::AllocationTooLarge => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Allocation too large"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::Interrupt => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Interrupt"
            ));
            InvokeErrorCode::Unrecoverable
        }

        // Component model related (which we should have off)
        wasmtime::Trap::CannotEnterComponent => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot enter component (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::CannotLeaveComponent => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot leave component (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::CannotBlockSyncTask => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot block sync task (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::InvalidChar => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Invalid char (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::StringOutOfBounds => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: String out of bounds (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::ListOutOfBounds => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: List out of bounds (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::InvalidDiscriminant => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Invalid discriminant (component model?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::UnalignedPointer => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Unaligned pointer (component model?)"));
            InvokeErrorCode::Unrecoverable
        }

        // Debug assertions (should only fire in debug builds)
        wasmtime::Trap::DebugAssertStringEncodingFinished => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Debug assert string encoding finished"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::DebugAssertEqualCodeUnits => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Debug assert equal code units"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::DebugAssertPointerAligned => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Debug assert pointer aligned"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::DebugAssertUpperBitsUnset => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Debug assert upper bits unset"
            ));
            InvokeErrorCode::Unrecoverable
        }

        // Async-related (should not occur in our synchronous execution model)
        wasmtime::Trap::NoAsyncResult => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: No async result (async?)"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::UnhandledTag => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Unhandled tag (async?)"
            ));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::ContinuationAlreadyConsumed => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Continuation already consumed (async?)"));
            InvokeErrorCode::Unrecoverable
        }
        wasmtime::Trap::AsyncDeadlock => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Async deadlock (async?)"
            ));
            InvokeErrorCode::Unrecoverable
        }

        // Disabled opcodes (should not occur if validation is correct)
        wasmtime::Trap::DisabledOpcode => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Disabled opcode (pulley?)"
            ));
            InvokeErrorCode::Unrecoverable
        }

        // Catch-all for any future trap variants (enum is non-exhaustive)
        _ => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Unknown trap: {trap:?}"
            ));
            InvokeErrorCode::Unrecoverable
        }
    }
}
