#pragma once

#include <cstdint>

namespace hogd {

/**
 * As specified in designdocs/syscall.md
 *
 * Enum is fully specified because this spec has to be replicated
 * (at least for the recoverable errors)
 * within contracts.
 */
enum class SyscallErrorCode : int32_t { // NOLINT

  kIsRootTransaction                = 0x0000'0001,
  kStorageObjectTypeMismatch        = 0x0000'0002,
  kStorageKeyNExist                 = 0x0000'0003,
  kStorageKeyAlreadyDeleted         = 0x0000'0004,

  kNonnegativeInt64SetValueMismatch = 0x0000'0005,

  kDoubleReturn                     = 0x0000'0006,

  kInvalidPtrError                  = 0x0000'0007,

  // anything negative is unrecoverable,
  // and should not be passed back to the runtime
  kUnrecoverableUnknown                    = INT32_MIN,
};

} // namespace hogd
