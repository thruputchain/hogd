#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/hog_exec/common/status.h"
#include "syscall_error.hh"

namespace hogd {

// Auth-token ids checked once for shape: whole uint32_t values, no repeats,
// contract order preserved because it is consensus-visible. Ownership is the
// frame stack's to check, not this type's.
class ValidatedAuthTokens {
public:
  ValidatedAuthTokens() = default;

  ValidatedAuthTokens(ValidatedAuthTokens const &) = delete;
  ValidatedAuthTokens &operator=(ValidatedAuthTokens const &) = delete;
  ValidatedAuthTokens(ValidatedAuthTokens &&) noexcept = default;
  ValidatedAuthTokens &operator=(ValidatedAuthTokens &&) noexcept = default;

  // Parses a contract-supplied byte span, which may be unaligned.
  // InvalidSyscallInput when the length is not a multiple of sizeof(uint32_t)
  // or an id repeats.
  //
  // Duplicates are found by sorting a scratch copy and scanning adjacent
  // values. The callers bound the list by kConfigParamMaxAuthTokens before
  // calling, so this sorts a few dozen ints at most. A hash set costs a node
  // allocation per id to avoid a sort that small, and needs a keyed hasher to
  // be safe on contract-chosen ids at all (#2788); the bound settles both.
  [[nodiscard]] static SyscallStatus<ValidatedAuthTokens>
  parse(std::span<const std::byte> bytes)
  {
    if (bytes.size() % sizeof(uint32_t) != 0) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    std::vector<uint32_t> parsed_ids(bytes.size() / sizeof(uint32_t));
    if (!bytes.empty()) {
      std::memcpy(parsed_ids.data(), bytes.data(), bytes.size());
    }
    std::vector<uint32_t> sorted_ids(parsed_ids);
    std::ranges::sort(sorted_ids);
    if (std::ranges::adjacent_find(sorted_ids) != sorted_ids.end()) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    return ValidatedAuthTokens(std::move(parsed_ids));
  }

  std::span<uint32_t const> ids() const { return ids_; }
  size_t size() const { return ids_.size(); }
  bool empty() const { return ids_.empty(); }

  // Hands the list to the frame stack, which owns it from then on.
  std::vector<uint32_t> release() && { return std::move(ids_); }

private:
  explicit ValidatedAuthTokens(std::vector<uint32_t> &&ordered_ids)
    : ids_(std::move(ordered_ids))
  {}

  std::vector<uint32_t> ids_;
};

static_assert(!std::is_copy_constructible_v<ValidatedAuthTokens>);
static_assert(std::is_nothrow_move_constructible_v<ValidatedAuthTokens>);
static_assert(std::is_nothrow_move_assignable_v<ValidatedAuthTokens>);

} // namespace hogd
