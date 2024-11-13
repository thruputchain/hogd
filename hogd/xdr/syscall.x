// clang-format off
%// *** Generated Code: ***
%// *** Do not check in ***

% #include "types.hh"

namespace hogd
{
  // clang-format on

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
  };

} // namespace hogd
