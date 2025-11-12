#include "hogd/hogdb/item_change.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "account.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

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
AuthDataItem::narrow(const AuthDataItem &auth_data_item) const
{
  CHECK(base_auth_data() == auth_data_item.base_auth_data());
  return true;
}

ReplayCacheIt::ReplayCacheIt(BlockHeight expired_height,
                             ReplayCacheData const &cache)
  : expired_height_{expired_height}, base_replay_cache_{}
{
  // prune expired entries right on construction so only live entries get
  // factored x23's append_range size check for reserve makes it hard to use
  // std::views, do things manually.
  base_replay_cache_.uniquifiers.reserve(cache.uniquifiers.size());
  for (Uniquifier const &uniquifier : cache.uniquifiers) {
    if (uniquifier.block_expiry_height <= expired_height)
      continue;
    base_replay_cache_.uniquifiers.push_back(std::move(uniquifier));
  }
}

HogDBStatus<void>
ReplayCacheIt::check_conflict(Uniquifier const &uniquifier)
{
  {
    auto it = std::ranges::find_if(additions_,
                                   [&uniquifier](Uniquifier const &addition) {
                                     return uniquifier.nonce == addition.nonce;
                                   });
    // nonce was in list of recent additions
    if (it != additions_.end())
      return HogDBError(HogDBCode::kConflict);
  }
  {
    auto it = std::ranges::find_if(base_replay_cache_.uniquifiers,
                                   [&uniquifier](Uniquifier const &base) {
                                     return base.nonce == uniquifier.nonce;
                                   });
    // nonce was in the base replay cache
    if (it != base_replay_cache_.uniquifiers.end())
      return HogDBError(HogDBCode::kConflict);
  }
  return {};
}

HogDBStatus<void>
ReplayCacheIt::try_consume_uniquifier(Uniquifier const &uniquifier)
{
  assert_is_valid();

  // reject uniquifiers that are used too late
  if (uniquifier.block_expiry_height <= expired_height_)
    return HogDBError(HogDBCode::kConflict);

  // reject uniquifiers that are already used
  HogDBStatus<void> ret = check_conflict(uniquifier);
  if (!ret)
    return ret;

  additions_.emplace_back(uniquifier);

  assert_is_valid();
  return {};
}

HogDBStatus<void>
ReplayCacheIt::commit(HogDBDAO &db, MerkleTriePrefix &trie,
                      const ItemKey &key) const
{
  assert_is_valid();

  auto res = build_result();
  auto ret = db.write_address_replay_cache(key.get_address(), res);
  if (!ret) {
    return ret.error();
  }
  auto buf = x23::serialize(res);
  CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, key, 0,
                                                  std::as_bytes(std::span(buf)))
            .has_value());
  return ret;
}

ReplayCacheData
ReplayCacheIt::build_result() const
{
  ReplayCacheData result = base_replay_cache_;
  std::ranges::copy(additions_, std::back_inserter(result.uniquifiers));
  return result;
}

int128_t
ReplayCacheIt::committed_storage_cost(const StorageCostModel *cost_model) const
{
  // try_consume_uniqufier never allows the addition of conflicting entries and
  // neither does merge. Hence, all uniquifiers in the change will be part
  // of its commit.
  return marginal_storage_cost(cost_model);
}

int128_t
ReplayCacheIt::marginal_storage_cost(const StorageCostModel *cost_model) const
{
  size_t entries = base_replay_cache_.uniquifiers.size() + additions_.size();
  return cost_model->get_replay_cache_cost(entries);
}

bool
ReplayCacheIt::narrow(const ReplayCacheIt &) // NOLINT - lint wants static
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

HogDBStatus<int128_t>
ReplayCacheIt::merge(const ReplayCacheIt &replay_cache,
                     const StorageCostModel *cost_model)
{
  assert_is_valid();
  replay_cache.assert_is_valid();

  if (base_replay_cache_ != replay_cache.base_replay_cache_)
    return HogDBError(HogDBCode::kConflict);

  // Check that incoming uniquifiers aren't in this cache or additions.
  for (auto const &other_addition : replay_cache.additions_) {
    HogDBStatus<void> ret = check_conflict(other_addition);
    if (!ret)
      return ret.error();
  }

  // Conflicts checked; now do merge
  std::ranges::copy(replay_cache.additions_, std::back_inserter(additions_));

  assert_is_valid();
  size_t entries =
      base_replay_cache_.uniquifiers.size() + replay_cache.additions_.size();
  return cost_model->get_replay_cache_cost(entries);
}

void
ReplayCacheIt::undo_merge(const ReplayCacheIt &replay_cache)
{
  assert_is_valid();
  replay_cache.assert_is_valid();

  auto removal_nonces =
      replay_cache.additions_ | std::views::transform(&Uniquifier::nonce);
  std::unordered_set<ReplayNonce> removals(removal_nonces.begin(),
                                           removal_nonces.end());
  std::erase_if(additions_, [&removals](Uniquifier const &addition) {
    return removals.contains(addition.nonce);
  });

  assert_is_valid();
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
