#pragma once

#include <cstdint>

namespace hogd {

/**
 * As specified in designdocs/syscall.md
 *
 * Enum is fully specified because this spec has to be replicated
 * within contracts.
 */
enum class KeyStatus : uint64_t { // NOLINT
  // Key exists, in the local view of a contract
  kKeyExists         = 0,
  // Key does not exist in the local view of a contract,
  // and never existed at any point during the invocation
  // of the contract
  kKeyNExist         = 1,
  // Key does not exist in the local view of a contract,
  // but existed at some point earlier during the invocation
  // of the contract.
  kKeyAlreadyDeleted = 2
};

} // namespace hogd
