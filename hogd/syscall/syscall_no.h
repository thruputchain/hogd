#pragma once

#include <cstdint>

namespace hogd {

/**
 * As specified in designdocs/syscall.md
 *
 * Enum is fully specified because this spec has to be replicated
 * within contracts.
 */
enum class SyscallNo : uint64_t { // NOLINT
  // general
  kGetMessageSender = 0x1000'0000,

  // crypto
  kSHA256           = 0x2000'0000,

  // general storage
  kHasKey           = 0x3000'0000,
  kDeleteLast       = 0x3000'0001,

};

} // namespace hogd
