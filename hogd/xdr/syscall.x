namespace hogd {

// Documentation in designdocs/syscall.md
enum SyscallNo {
  kGetMsgSender = 0,
  kSHA256 = 1,

  // storage obj read/write
  kHasKey = 1000,
  kDeleteLast = 1001,

  kBytestringSet = 1100,
  kBytestringGet = 1101,
  kBytestringGetLen = 1102,

  kNNInt64SetAdd = 1200,
  kNNInt64Get = 1201,
  kNNInt64GetBase = 1202,

  // invoke
  kGetCalldata = 2000,
  kGetCalldataLen = 2001,
  kReturn = 2002,
  kInvoke = 2003,
  kGetFullReturn = 2004,
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
  // Call return twice in one function
  kDoubleReturn = 6,

  // Call get_msg_sender on root transaction
  kIsRootTransaction = 7,
  kUnknownSyscall = 8,
};

} // namespace hogd
