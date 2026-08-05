#include "hogd/hog_db/item_change.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <expected>
#include <type_traits>
#include <variant>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/status.h"

namespace hogd {

HogDbStatus<bool>
NNInt128::narrow(const NNInt128 &nnint)
{
  assert_is_valid();
  nnint.assert_is_valid();

  // base must be the same to narrow.
  if (base_ != nnint.base_)
    return false;

  // narrow the bounds based on the greatest restriction.
  int128_t plus = std::max(plus_, nnint.plus_);
  int128_t minus = std::min(minus_, nnint.minus_);

  // check if could go negative.
  if (base_ + minus < 0)
    return false;

  // check if could overflow.
  auto [ign, of] = overflowing_add(base_, plus);
  if (of)
    return false;

  // update bounds.
  plus_ = plus;
  minus_ = minus;

  assert_is_valid();
  return true;
}

HogDbStatus<bool>
Balance::narrow(const Balance &other)
{
  assert_is_valid();
  other.assert_is_valid();
  add_ = std::max(add_, other.add_);
  sub_ = std::min(sub_, other.sub_);
  assert_is_valid();
  return true;
}

HogDbStatus<bool>
AuthDataItem::narrow(const AuthDataItem &auth_data_item) const
{
  if (base_auth_data() != auth_data_item.base_auth_data()) {
    return HogDbError(HogDbCode::kStaleBlock);
  }
  return true;
}

size_t
AuthDataItem::count_active_keys() const
{
  size_t active_keys = 0;
  for (size_t i = 0; i < kMaxSigners; ++i) {
    int base = (base_auth_data().public_keys[i] != nullptr) ? 1 : 0;
    if (pk_changes_[i].has_value()) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      base = ((*pk_changes_[i]) != nullptr) ? 1 : 0;
    }
    active_keys += base;
  }
  return active_keys;
}

HogDbStatus<bool>
ReplayCacheIt::narrow( // NOLINT - lint wants static
    const ReplayCacheIt & /*unused*/)
{
  // Currently, the executor only modifies a replay cache based on the
  // uniquifier assigned to a transaction right at its start and only once.
  // Since a single transaction should never be able to modify a replay cache
  // twice, narrow() shouldn't be possible on it. Later, if we allow
  // transactions to interact with replay caches then this will need to be
  // implemented.
  CHECK(false);
  return false;
}

HogDbStatus<bool>
RetrocausalQueueIt::narrow(RetrocausalQueueIt const &earlier)
{
  // Track the high-water mark of insert count across all subtransactions.
  // The executor's get_delta() prunes self-consumed entries from the front
  // of the insert list, so the raw size can shrink. The max preserves the
  // peak for reservation accounting in RetrocausalQueueLock.
  max_insert_count_ = std::max(max_insert_count_, earlier.max_insert_count_);

  // The cumulative delta's consume can only grow within a transaction:
  // each frame inherits the prior frame's ItemView state (including consume),
  // and the single-consumer constraint means only one tx per block can consume.
  // An honest executor never drops or reduces consumes.
  if (earlier.delta_.consume && !delta_.consume) {
    return std::unexpected(HogDbError{HogDbCode::kInvalidExecutorBehavior});
  }

  if (earlier.delta_.consume && delta_.consume) {
    auto const &ec = earlier.delta_.consume->past_block_consume;
    auto const &nc = delta_.consume->past_block_consume;
    bool monotonic =
        (ec < nc) ||
        (ec == nc && earlier.delta_.consume->current_block_consumed.size() <=
                         delta_.consume->current_block_consumed.size());
    if (!monotonic) {
      return std::unexpected(HogDbError{HogDbCode::kInvalidExecutorBehavior});
    }
  }
  return true;
}

namespace {

template<typename T>
concept is_monostate = std::same_as<std::decay_t<T>, std::monostate>;

} // namespace

HogDbStatus<void>
ItemChange::narrow(const ItemChange &change)
{
  if (type_.index() != change.type_.index() || change.exclusive_) {
    exclusive_ = true;
    return {};
  }

  auto result = std::visit(
      [&change](auto &arg) -> HogDbStatus<bool> {
        if constexpr (!is_monostate<decltype(arg)>)
          return arg.narrow(
              std::get<std::decay_t<decltype(arg)>>(change.type_));
        return true;
      },
      type_);
  if (!result)
    return std::unexpected(result.error());
  exclusive_ = exclusive_ || !*result;
  return {};
}

} // namespace hogd
