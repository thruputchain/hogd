
% #include "types.hh"

    namespace hogd
{

  enum SyscallNo {
    kSHA256 = 0,
    kGetMsgSender = 1,
    kGetCalldataLen = 2,
    kGetCalldata = 3,
    kLog = 4,
    kInvoke = 5,
    kReturn = 6,
    kGetReturnedBuffer = 7,
    kGetDeployAddress = 8,
    kDeployContract = 9,
    kGetSourceTransactionHash = 10,
    kExit = 11,
  };

  enum SyscallErrorCode {
    // No error
    kNone = 0,

    // Out of bounds pointer to wasm memory passed to syscall
    kFault = 1,

    // Unknown syscall number
    kUnknownSyscall = 2,

    // Cannot call get_msg_sender on root invocation
    kIsRootTransaction = 3,

    // Failed to find contract
    kContractLookupFailed = 4,

    // Input byte span is too long
    kStringTooLong = 5,

    // No return buffer is available.  Either the most recent subcall failed, or
    // there have been no subcalls within this call frame.
    kNoReturnBufferAvailable = 6,

    // Insufficient gas in call frame to supply requested gas limit for child
    // invocation
    kInsufficientGasForInvocation = 7,

    // Attempt made to deploy a contract twice (same nonce and contract hash,
    // from same address)
    kContractAlreadyDeployed = 8,

    // Deterministic error within a wasm invocation
    kDeterministicWasm = 9,
  };

} // namespace hogd
