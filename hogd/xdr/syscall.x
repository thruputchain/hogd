
%#include "types.hh"

namespace hogd {

enum SyscallNo {
  kSHA256 = 0,
  kGetMsgSender = 1,
  kBytestringGetLen = 2,
  kBytestringGet = 3,
  kBytestringSet = 4,
  kHasKey = 5,
};

enum SyscallErrorCode {
  // No error
  kNone = 0,

  // Out of bounds pointer to wasm memory passed to syscall
  kFault = 1,

  // Syscall's expected type does not match object type at provided key
  kStorageObjectTypeMismatch = 2,

  // Access to storage element that does not exist. Writes to this key can be
  // accepted in the current transaction.
  kStorageKeyNExist = 3,

  // Access to storage element that has already been deleted in the current
  // transaction. Further writes to this key will never be accepted within the
  // current transaction.
  kStorageKeyAlreadyDeletedInCurrentTransaction = 4,

  // Unknown syscall number
  kUnknownSyscall = 5,

  // Cannot set a nonnegative int to different values in the same transaction
  kNonnegativeIntSetValueMismatch = 6,
};

} // namespace hogd
