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
    match classify_trap(trap) {
        Some(code) => code,
        None => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: Unknown trap: {trap:?}"
            ));
            InvokeErrorCode::Unrecoverable
        }
    }
}

// Returns None for unclassified traps (i.e. new variants added by a wasmtime
// upgrade that we haven't explicitly handled yet).
fn classify_trap(trap: &wasmtime::Trap) -> Option<InvokeErrorCode> {
    Some(match trap {
        // Gas/fuel related - expected during normal operation
        wasmtime::Trap::OutOfFuel => InvokeErrorCode::OutOfGas,

        // Deterministic errors - these are expected from bad contract code
        wasmtime::Trap::MemoryOutOfBounds
        | wasmtime::Trap::TableOutOfBounds
        | wasmtime::Trap::IndirectCallToNull
        | wasmtime::Trap::BadSignature
        | wasmtime::Trap::IntegerOverflow
        | wasmtime::Trap::IntegerDivisionByZero
        | wasmtime::Trap::BadConversionToInteger
        | wasmtime::Trap::UnreachableCodeReached
        | wasmtime::Trap::NullReference
        | wasmtime::Trap::ArrayOutOfBounds
        | wasmtime::Trap::CastFailure => InvokeErrorCode::Deterministic,

        // Everything below should not occur during normal groundhog operation.
        // Stack/memory
        wasmtime::Trap::StackOverflow
        | wasmtime::Trap::HeapMisaligned
        | wasmtime::Trap::AtomicWaitNonSharedMemory
        | wasmtime::Trap::AllocationTooLarge
        | wasmtime::Trap::Interrupt
        // Component model (disabled)
        | wasmtime::Trap::CannotEnterComponent
        | wasmtime::Trap::CannotLeaveComponent
        | wasmtime::Trap::CannotBlockSyncTask
        | wasmtime::Trap::InvalidChar
        | wasmtime::Trap::StringOutOfBounds
        | wasmtime::Trap::ListOutOfBounds
        | wasmtime::Trap::InvalidDiscriminant
        | wasmtime::Trap::UnalignedPointer
        // Component-model async tasks (disabled)
        | wasmtime::Trap::TaskCancelNotCancelled
        | wasmtime::Trap::TaskCancelOrReturnTwice
        | wasmtime::Trap::SubtaskCancelAfterTerminal
        | wasmtime::Trap::TaskReturnInvalid
        | wasmtime::Trap::WaitableSetDropHasWaiters
        | wasmtime::Trap::SubtaskDropNotResolved
        | wasmtime::Trap::BackpressureOverflow
        | wasmtime::Trap::UnsupportedCallbackCode
        | wasmtime::Trap::ConcurrentFutureStreamOp
        // Debug assertions
        | wasmtime::Trap::DebugAssertStringEncodingFinished
        | wasmtime::Trap::DebugAssertEqualCodeUnits
        | wasmtime::Trap::DebugAssertPointerAligned
        | wasmtime::Trap::DebugAssertUpperBitsUnset
        // Async/continuations (synchronous execution model)
        | wasmtime::Trap::NoAsyncResult
        | wasmtime::Trap::UnhandledTag
        | wasmtime::Trap::ContinuationAlreadyConsumed
        | wasmtime::Trap::AsyncDeadlock
        // Threading/stack-switching (disabled)
        | wasmtime::Trap::CannotResumeThread
        | wasmtime::Trap::ThreadNewIndirectInvalidType
        | wasmtime::Trap::ThreadNewIndirectUninitialized
        // Pulley interpreter
        | wasmtime::Trap::DisabledOpcode => {
            debug_log!(eprintln!(
                "Unrecoverable: Unexpected Wasmtime trap: {trap:?}"
            ));
            InvokeErrorCode::Unrecoverable
        }

        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Iterates every wasmtime Trap discriminant and asserts classify_trap
    /// returns Some for each one. When a wasmtime upgrade adds a new variant,
    /// classify_trap's catch-all returns None and this test fails — forcing
    /// explicit classification.
    #[test]
    fn all_wasmtime_traps_explicitly_classified() {
        for byte in 0..=u8::MAX {
            let Some(trap) = wasmtime::Trap::from_u8(byte) else {
                continue;
            };
            assert!(
                classify_trap(&trap).is_some(),
                "unclassified wasmtime trap variant: {trap:?} (discriminant {byte}). \
                 Add an explicit match arm in classify_trap().",
            );
        }
    }
}
