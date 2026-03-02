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
        Self::error(wasmtime_trap_to_invoke_error(trap))
    }
}

pub fn wasmtime_trap_to_invoke_error(trap: &wasmtime::Trap) -> InvokeErrorCode {
    crate::debug_log!(eprintln!("Wasmtime trap: {trap:?}"));
    match trap {
        // Gas/fuel related - expected during normal operation
        wasmtime::Trap::OutOfFuel => InvokeErrorCode::kInvokeOutOfGas,

        // Deterministic errors - these are expected from bad contract code
        wasmtime::Trap::MemoryOutOfBounds => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::TableOutOfBounds => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::IndirectCallToNull => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::BadSignature => InvokeErrorCode::kInvokeDeterministic,
        wasmtime::Trap::IntegerOverflow => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::IntegerDivisionByZero => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::BadConversionToInteger => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::UnreachableCodeReached => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::NullReference => InvokeErrorCode::kInvokeDeterministic,
        wasmtime::Trap::ArrayOutOfBounds => {
            InvokeErrorCode::kInvokeDeterministic
        }
        wasmtime::Trap::CastFailure => InvokeErrorCode::kInvokeDeterministic,

        // Errors that should not occur during normal groundhog operation
        // These indicate serious system issues or bugs
        wasmtime::Trap::StackOverflow => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Stack overflow"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::HeapMisaligned => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Heap misaligned (atomic op?)"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::AtomicWaitNonSharedMemory => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Atomic wait on non-shared memory"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::AllocationTooLarge => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Allocation too large"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::Interrupt => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Interrupt"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }

        // Component model related (which we should have off)
        wasmtime::Trap::CannotEnterComponent => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot enter component (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::CannotLeaveComponent => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot leave component (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::CannotBlockSyncTask => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Cannot block sync task (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::InvalidChar => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Invalid char (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::StringOutOfBounds => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: String out of bounds (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::ListOutOfBounds => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: List out of bounds (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::InvalidDiscriminant => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Invalid discriminant (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::UnalignedPointer => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Unaligned pointer (component model?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }

        // Debug assertions (should only fire in debug builds)
        wasmtime::Trap::DebugAssertStringEncodingFinished => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Debug assert string encoding finished"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::DebugAssertEqualCodeUnits => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Debug assert equal code units"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::DebugAssertPointerAligned => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Debug assert pointer aligned"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::DebugAssertUpperBitsUnset => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Debug assert upper bits unset"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }

        // Async-related (should not occur in our synchronous execution model)
        wasmtime::Trap::NoAsyncResult => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: No async result (async?)"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::UnhandledTag => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Unhandled tag (async?)"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::ContinuationAlreadyConsumed => {
            debug_log!(eprintln!("Unrecoverable: Unexpected Wasmtime trap: Continuation already consumed (async?)"));
            InvokeErrorCode::kInvokeUnrecoverable
        }
        wasmtime::Trap::AsyncDeadlock => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Async deadlock (async?)"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }

        // Disabled opcodes (should not occur if validation is correct)
        wasmtime::Trap::DisabledOpcode => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Disabled opcode (pulley?)"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }

        // Catch-all for any future trap variants (enum is non-exhaustive)
        _ => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Unknown trap: {trap:?}"
            ));
            InvokeErrorCode::kInvokeUnrecoverable
        }
    }
}
