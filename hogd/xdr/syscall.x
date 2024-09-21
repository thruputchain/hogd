namespace hogd {

// Documentation in designdocs/syscall.md
enum SyscallNo {
  kSHA256 = 0,
  kGetMsgSender = 1,

  // storage obj read/write
  kHasKey = 2,
  kDeleteLast = 3,

  kBytestringSet = 4,
  kBytestringGet = 5,
  kBytestringGetLen = 6,

  kNNInt64SetAdd = 7,
  kNNInt64Get = 8,
  kNNInt64GetBase = 9,

  // invoke
  kGetCalldata = 10,
  kGetCalldataLen = 11,
  kReturn = 12,
  kInvoke = 13,
  kGetFullReturn = 14,
};

enum SyscallErrorCode {
  kNone = 0,

  // an argument passed to the syscall is an invalid
  // ptr into wasm memory
  kInvalidPtrError = 1,

  // General State Access Errors
  kStorageObjectTypeMismatch = 2,
  kStorageKeyNExist = 3,
  kStorageKeyAlreadyDeleted = 4,

  kNonnegativeInt64SetValueMismatch = 5,

  // Invocation Errors

  // Call get_msg_sender on root transaction
  kIsRootTransaction = 6,
  kUnknownSyscall = 7,
};

} // namespace hogd
