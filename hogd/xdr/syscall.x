
// clang-format off
%#include "types.hh"

namespace hogd {
  // clang-format on

  enum SyscallNo {
    kSHA256 = 0,
    kGetMsgSender = 1,
    kBytestringGetLen = 2,
    kBytestringGet = 3,
    kBytestringSet = 4,
    kHasKey = 5,
    kDeleteLast = 6,
  };

  enum SyscallErrorCode {
    // No error
    kNone = 0,

    // Out of bounds pointer to wasm memory passed to syscall
    kFault = 1,

    // Syscall's expected type does not match object type at provided key
    kStorageObjectTypeMismatch = 2,

    // Access to storage objet that does not logically exist. An object exists
    // if it exists at the start of a transaction (in the ledger snapshot), or
    // if it is created by an operation in that transaction.  The DeleteLast
    // syscall is not factored into this calculation, as those deletions are
    // only applied at the end of a block.
    kStorageKeyNExist = 3,

    // Unknown syscall number
    kUnknownSyscall = 4,

    // Cannot set a nonnegative int to different values in the same transaction
    kNonnegativeIntSetValueMismatch = 5,

    // Cannot call get_msg_sender on root invocation
    kIsRootTransaction = 6,

    // Failed to find contract
    kContractLookupFailed = 7,

    // Input to set_bytestring is too long for one bytestring
    kBytestringTooLong = 8,

    // Deterministic error within called wasm (i.e. trap)
    kDeterministicWasmError = 9,
  };

} // namespace hogd
