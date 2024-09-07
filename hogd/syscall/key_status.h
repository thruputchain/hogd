#pragma once

#include <cstdint>

namespace hogd
{

/**
 * As specified in designdocs/syscall.md
 *
 * Enum is fully specified because this spec has to be replicated
 * within contracts.
 */
enum class KeyStatus : uint64_t { // NOLINT
  kKeyExists = 0,
  kKeyNExist = 1,
  kKeyAlreadyDeleted = 2
};

}
