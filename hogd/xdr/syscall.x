%// *** Generated Code: ***
%// *** Do not check in ***

% #include "types.hh"

    namespace hogd
{
  enum SyscallNo {
    kSHA256 = 0,
  };

  enum SyscallErrorCode {
    // No error
    kNone = 0,

    // Out of bounds pointer to wasm memory passed to syscall
    kFault = 1,

    // System call number is not supported
    kNoSyscall = 2,

    // Invalid syscall parameters
    kInvalidSyscallInput = 3,

    // Mismatch between syscall expected object type and actual object type
    kStorageObjectTypeMismatch = 4,

    // Accessing nonexistent object
    kStorageKeyNExist = 5,
  };

} // namespace hogd
