// clang-format off

namespace hogd {
// clang-format on

enum SyscallErrorCode {
  // No error
  kNone = 0,

  // Out of bounds pointer to wasm memory passed to syscall
  kFault = 1,

  // System call for supplied number is not supported
  kNoSyscall = 2,

  // Cannot call get_msg_sender on root invocation
  kIsRootTransaction = 3,

  // Failed to find contract
  kContractLookupFailed = 4,

  // Attempt made to deploy a contract twice (same nonce and contract hash, from
  // same address)
  kContractAlreadyDeployed = 5,

  // No return buffer is available.  Either the most recent subcall failed, or
  // there have been no subcalls within this call frame.
  kNoReturnBufferAvailable = 6,

  // Insufficient gas in call frame to supply requested gas limit for child
  // invocation
  kInsufficientGasForInvocation = 7,

  // Invalid input to a syscall, such as subtracting below 0 on an nnint64
  kInvalidSyscallInput = 8,

  // Accessing a storage key that does not exist in the current view
  kStorageKeyNotExist = 9,

  // No entry in the retrocausal queue to consume
  kNoRetrocausalQueueEntry = 10,

  // Failed to parse calldata
  kCalldataParseError = 100,

  // Failed to parse returned data
  kReturnParseError = 101,

  // Deterministic error within called wasm (i.e. trap)
  kDeterministicWasm = -1,

  // Child call frame manually reverted
  kFrameReverted = -2,
};

} // namespace hogd
