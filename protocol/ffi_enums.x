namespace hogd {

// Error codes returned from a runtime (wasmi) invocation.
// This is the external interface to a smart contract. Normally wrapped in an
// InvokeStatus<int32_t> (a std::expected).
//
// When InvokeStatus<int32_t> has an int32_t value (and an
// InvokeErrorCode::kNone), the int32_t value represents a handleable
// (deterministic) result from the invocation. (Specifically, the invoke()
// syscall in a nested smart contract invocation passes an
// {InvokeErrorCode::kNone, code} back as a
// {HostFnErrorCode::kNoneOrRecoverable, code}).
//
// This value is generated as either the returned value from the selected
// WebAssembly exported function that was run, the code caused by a host
// function returning HostFnErrorCode::kExit, or 0, if invocation ends because a
// host function returns HostFnErrorCode::kReturn.
enum InvokeErrorCode {
  // A call frame can only succeed if this code is returned.
  // All others revert the call frame.
  kInvokeNone = 0,
  // An unspecified deterministic error.
  kInvokeDeterministic = 1,
  // Runtime exceeded its gas budget.
  kInvokeOutOfGas = 2,
  // Nondeterministic error during invocation (e.g. malloc error)
  // Signals a serious system issue or a serious bug in our code and should be
  // diagnosed ASAP.
  kInvokeUnrecoverable = 3,
  // Discovered during validation that proposer submitted bad data.
  // If a block contains a transaction with this error, the block does not need
  // to be discarded.  The transaction that causes this error is discarded,
  // and the block proposer should be penalized (via whatever out of band
  // mechanism),
  // but the rest of the block can still be processed.
  // Note that this is different from the case of a block proposal with
  // conflicting transactions;
  // those errors are discovered _after_ executing the transactions in a block,
  // and require
  // discarding the entire block.
  kInvokeValidationFailure = 4,
  // The transaction was aborted due to an external reason (such as the current
  // block being discarded),
  // but this is not the result of a totally unrecoverable error. This is to be
  // expected during normal system operation.
  kInvokeAbortTransaction = 5,
};

// HostFunctionErrorCode categorizes errors returned from system calls. The
// normal usage pattern is to wrap them within HostFnStatus<int32_t>, where the
// int32 is the more detailed error code.
//
// In WebAssembly, a host function is defined as a function that can be
// called outside of WebAssembly. We import one of these functions to provide
// access to our table of system call handlers.
//
// When a HostFnStatus<int32_t> (a wrapper around std::expected) has an int32_t
// value, (and a HostFnErrorCode::kNoneOrRecoverable), that int32_t is passed to
// WebAssembly as a handleable error code, as the return result from the host
// function. The WebAssembly bytecode (the smart contract) itself never sees the
// HostFnErrorCode. Other HostFnErrorCodes terminate the WebAssembly runtime
// (returning to the caller), and are handled by the WebAssembly runtime.
enum HostFnErrorCode {
  kHostNoneOrRecoverable = 0,
  // Insufficient gas to continue WebAssembly execution.
  //
  // Result: the WebAssembly runtime immediately returns an
  // InvokeErrorCode::kHostOutOfGas.
  kHostOutOfGas = 1,
  // Unrecoverable (nondeterministic) error, such as a malloc error.
  // The only course of action is to abort the entire transaction.
  //
  // Result: the WebAssembly runtime immediately returns an
  // InvokeErrorCode::kUnrecoverable
  // Signals a serious system issue or a serious bug in our code and should be
  // diagnosed ASAP.
  kHostUnrecoverable = 2,
  // Return from a WebAssembly invocation to the caller.  Registers a byte
  // buffer to be passed back to the caller.
  //
  // Result: Runtime immediately returns an {InvokeErrorCode::kNone, 0}.
  kHostReturn = 3,
  // Return from a WebAssembly invocation to the caller, with some (int32_t)
  // error code.
  //
  // Result: Runtime immediately returns an {InvokeErrorCode::kNone, code}.
  //
  // On a root smart contract invocation, a nonzero code is converted to a
  // InvokeErrorCode::kDeterministic.
  kHostExit = 4,
  // Validation fails because a proposer submitted bad nondeterministic
  // proposal data.
  //
  // Result: the WebAssembly runtime immediately returns an
  // InvokeErrorCode::kValidationFailure
  kHostValidationFailure = 5,
  // The transaction was aborted due to an external reason (such as the current
  // block being discarded), but this is not the result of a totally
  // unrecoverable error. This is to be expected during normal system operation.
  //
  // Result: the WebAssembly runtime immediately returns an
  // InvokeErrorCode::kAbortTransaction
  kHostAbortTransaction = 6,
};

} // namespace hogd
