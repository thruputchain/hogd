
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

    // Unknown syscall number
    kUnknownSyscall = 2,
  };

} // namespace hogd
