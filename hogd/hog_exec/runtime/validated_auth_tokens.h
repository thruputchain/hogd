#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "syscall_error.hh"

namespace hogd {

// Keyed hasher for auth-token ids: the ids are contract-chosen, so a default
// integer hash could be packed into one bucket (#2788).
struct AuthTokenIdHasher {
  std::size_t operator()(uint32_t token_id) const noexcept
  {
    return SipHash24::get_default()(std::as_bytes(std::span(&token_id, 1)));
  }
};

using AuthTokenIdSet = std::unordered_set<uint32_t, AuthTokenIdHasher>;

// Per-process hash order in AuthTokenIdSet must never reach consensus-visible
// state; ids() is the ordered form for that.
//
// Auth-token ids checked once for shape: whole uint32_t values, no repeats.
// ids() preserves contract order (consensus-visible); set() is for ownership
// lookups. Ownership itself is checked by the frame stack, not here.
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
  static SyscallStatus<ValidatedAuthTokens>
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
    AuthTokenIdSet id_set;
    id_set.reserve(parsed_ids.size());
    for (uint32_t id : parsed_ids) {
      if (!id_set.insert(id).second) {
        return SyscallError{
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
      }
    }
    return ValidatedAuthTokens(std::move(parsed_ids), std::move(id_set));
  }

  std::span<uint32_t const> ids() const { return ids_; }
  AuthTokenIdSet const &set() const { return set_; }
  size_t size() const { return ids_.size(); }
  bool empty() const { return ids_.empty(); }

  // Hands both forms to the frame stack, which owns them from then on.
  std::pair<std::vector<uint32_t>, AuthTokenIdSet> release() &&
  {
    return {std::move(ids_), std::move(set_)};
  }

private:
  ValidatedAuthTokens(std::vector<uint32_t> &&host_ids, AuthTokenIdSet &&id_set)
    : ids_(std::move(host_ids)), set_(std::move(id_set))
  {}

  std::vector<uint32_t> ids_;
  AuthTokenIdSet set_;
};

static_assert(!std::is_copy_constructible_v<ValidatedAuthTokens>);
static_assert(std::is_nothrow_move_constructible_v<ValidatedAuthTokens>);
static_assert(std::is_nothrow_move_assignable_v<ValidatedAuthTokens>);

} // namespace hogd
