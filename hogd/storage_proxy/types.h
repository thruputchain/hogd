#pragma once

#include <array>
#include <cstdint>
#include <variant>

namespace hogd {

// This code should be entirely deleted once XDR gets merged

// To be replaced within XDR
// Syscall defined in designdocs/syscall.md
enum class KeyStatus : uint64_t {
  kKeyExists = 0,
  kKeyNExist = 1,
  kKeyAlreadyDeleted = 2,
};

// Recoverable errors that can be passed back to a VM instance
// Specifics defined in designdocs/syscall.md.
enum class SyscallErrorCode : uint64_t { // NOLINT
  kStorageKeyNExist = 0x0000'0001,
  kStorageKeyAlreadyDeleted = 0x0000'0002,
};

// Will be XDR type
using AddressAndKey = std::array<uint8_t, 64>;

// Will be defined in XDR, eventually
class StorageObject {
  // many useful fields go here
};

}; // namespace hogd
