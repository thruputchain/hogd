#include "hogd/hogdb/item_change.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <variant>

#include "account.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

bool
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

bool
Balance::narrow(const Balance &other)
{
  assert_is_valid();
  other.assert_is_valid();
  add_ = std::max(add_, other.add_);
  sub_ = std::min(sub_, other.sub_);
  assert_is_valid();
  return true;
}

bool
AuthDataItem::narrow(const AuthDataItem &auth_data_item) const
{
  CHECK(base_auth_data() == auth_data_item.base_auth_data());
  return true;
}

bool
ReplayCacheIt::narrow( // NOLINT - lint wants static
    const ReplayCacheIt & /*unused*/)
{
  // Currently, the executor only modifies a replay cache based on the
  // uniquifier assigned to a transaction right at its start and only once.
  // Since a single transaction should never be able to modify a replay cache
  // twice, narrow() shouldn't be possible on it. Later, if we allow
  // transactions to interact with replay caches then this will need to be
  // implemented.
  assert(false);
  return false;
}

namespace {

template<typename T>
concept is_monostate = std::same_as<std::decay_t<T>, std::monostate>;

} // namespace

void
ItemChange::narrow(const ItemChange &change)
{
  if (type_.index() != change.type_.index() || change.exclusive_) {
    exclusive_ = true;
    return;
  }

  exclusive_ = exclusive_ ||
               !std::visit(
                   [&change](auto &arg) -> bool {
                     if constexpr (!is_monostate<decltype(arg)>)
                       return arg.narrow(
                           std::get<std::decay_t<decltype(arg)>>(change.type_));
                     return {};
                   },
                   type_);
}

} // namespace hogd
