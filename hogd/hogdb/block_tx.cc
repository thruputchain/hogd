#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/auth_lock.h"
#include "hogd/hogdb/item_lock/balance_lock.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/new_contract_lock.h"
#include "hogd/hogdb/item_lock/new_deployment_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "sched_db.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

namespace {

bool
is_state_prefix(std::byte prefix)
{
  switch (static_cast<Prefix>(prefix)) {
  case Prefix::kBalance:
  case Prefix::kAddressAuthData:
  case Prefix::kAddressMetadata:
  case Prefix::kContractDataKey:
  case Prefix::kWasmCodeRefcount:
  case Prefix::kWasmCode:
  case Prefix::kReplayCache:
  case Prefix::kRetrocausalQueueBlock:
    return true;
  case Prefix::kHogAge:
  case Prefix::kGlobalMetadata:
    return false;
  default:
    return false;
  }
}

void
lthash_add_item(LtHash16 &hash, std::span<const std::byte> key, uint8_t type,
                std::span<const std::byte> value)
{
  if (key.empty() || !is_state_prefix(key[0])) {
    return;
  }
  std::vector<std::byte> element;
  element.reserve(key.size() + 1 + value.size());
  element.insert(element.end(), key.begin(), key.end());
  element.push_back(static_cast<std::byte>(type));
  element.insert(element.end(), value.begin(), value.end());
  hash.add(element);
}

void
lthash_remove_item(LtHash16 &hash, std::span<const std::byte> key, uint8_t type,
                   std::span<const std::byte> value)
{
  if (key.empty() || !is_state_prefix(key[0])) {
    return;
  }
  std::vector<std::byte> element;
  element.reserve(key.size() + 1 + value.size());
  element.insert(element.end(), key.begin(), key.end());
  element.push_back(static_cast<std::byte>(type));
  element.insert(element.end(), value.begin(), value.end());
  hash.remove(element);
}

void
replace_known_item(LtHash16 &hash, HogDBDAO &db, std::span<const std::byte> key,
                   uint8_t type, std::span<const std::byte> new_value)
{
  auto before = db.get_ramdb().get(key);
  if (before.has_value()) {
    lthash_remove_item(hash, key, before->type, before->value);
  }
  lthash_add_item(hash, key, type, new_value);
}

auto
make_address_key_with_prefix(const Address &addr, Prefix prefix)
{
  std::array<std::byte, kAddressSize + 1> ret{};
  ret[0] = static_cast<std::byte>(prefix);
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1);
  return ret;
}

auto
make_contract_data_db_key(std::span<const std::byte> key)
{
  std::vector<std::byte> out;
  out.reserve(key.size() + 1);
  out.push_back(static_cast<std::byte>(Prefix::kContractDataKey));
  out.insert(out.end(), key.begin(), key.end());
  return out;
}

} // namespace

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_lock(ItemLock &lock, const ItemChange &change,
                 const LockContext &ctx)
{
  return lock.try_atomic_change_lock(nullptr, &change, ctx);
}

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_modify_lock(ItemLock &lock, const ItemChange &old_change,
                        const ItemChange &new_change, const LockContext &ctx)
{
  return lock.try_atomic_change_lock(&old_change, &new_change, ctx);
}

ATTRIBUTE_ALWAYS_INLINE void
BlockTX::do_unlock_item(ItemLock &lock, const ItemChange &change,
                        const LockContext &ctx)
{
  auto r = lock.try_atomic_change_lock(&change, nullptr, ctx);
  CHECK(r.has_value());
}

void
BlockTX::release_balance(const Address &addr, int128_t delta)
{
  assert(delta > 0);

  ItemKey key = make_address_balance_item_key(addr);
  auto *lock = addr_balance_locks_.insert_or_get(addr, [&] {
    return get_snapshot_cost_and_cache_data(key);
  });
  CHECK_PTR(lock);

  ItemChange change = ItemChange(Balance(delta));

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  if (auto r = do_lock(*lock, change, ctx); !r) [[unlikely]] {
    throw std::runtime_error("failed to lock balance");
  }

  lock->commit_change(change, ctx);
}

HogDBStatus<std::pair<int128_t, ItemLockCacheData>>
BlockTX::get_snapshot_cost_and_cache_data(const ItemKey &key)
{
  auto snapshot_cost =
      db_.get_charged_storage_cost_and_cache_data(key, &cost_model_);
  if (!snapshot_cost) {
    if (snapshot_cost.error() != HogDBCode::kNotFound) {
      return snapshot_cost.error();
    }
    assert(snapshot_cost.error() == HogDBCode::kNotFound);
    assert(!key.is_address_balance());

    return std::make_pair(0, ItemLockCacheData::from_none());
  }
  return *snapshot_cost;
}

HogDBStatus<int128_t>
BlockTX::lock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {

    auto *lock = addr_balance_locks_.insert_or_get(key.get_address(), [&] {
      return get_snapshot_cost_and_cache_data(
          make_address_balance_item_key(key.get_address()));
    });

    LockContext ctx(&db_, block_height_, key, &cost_model_);
    return do_lock(*lock, change, ctx)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *item = item_locks_.insert_or_get(key, [&] {
    return get_snapshot_cost_and_cache_data(key);
  });

  assert(item != nullptr);

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  return do_lock(*item, change, ctx);
}

HogDBStatus<int128_t>
BlockTX::modify_lock(const ItemKey &key, const ItemChange &old_change,
                     const ItemChange &new_change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    return do_modify_lock(*lock, old_change, new_change, ctx)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  return do_modify_lock(*lock, old_change, new_change, ctx);
}

void
BlockTX::unlock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    do_unlock_item(*lock, change, ctx);
    return;
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);
  LockContext ctx(&db_, block_height_, key, &cost_model_);
  do_unlock_item(*lock, change, ctx);
}

int128_t
BlockTX::commit_change(const ItemKey &key, ItemChange const &change)
{
  // TODO(geoff): In many cases we can evaluate here whether or not the item
  // change commit_change is a no-op.  In such cases, we can avoid acquiring the
  // lock.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    lock->commit_change(change, ctx);
    return 0;
  }
  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  return lock->commit_change(change, ctx);
}

HogDBStatus<std::optional<QueueEntry>>
BlockTX::get_concurrent_rcq_entry(const ItemKey &key, uint32_t index)
{
  auto *lock = item_locks_.get(key);
  if (!lock) {
    // Key was deleted and recreated as an RCQ in the same tx, or the lock
    // doesn't exist yet. A freshly-created queue has no concurrent entries.
    return std::nullopt;
  }
  return lock->get_concurrent_entry(index);
}

HogDBStatus<int128_t>
BlockTX::read_nnint128_base(const ItemKey &key)
{
  auto *lock = item_locks_.get(key);
  if (!lock) {
    auto r = db_.read_nnint128(key);
    if (!r) {
      if (r.error() == HogDBCode::kNotFound ||
          r.error() == HogDBCode::kTypeMismatch) {
        return 0;
      }
      return r.error();
    }
    return *r;
  }

  auto base = lock->get_cached_nnint128_base();
  if (base) {
    return *base;
  }
  return 0;
}

HogDBStatus<int128_t>
BlockTX::read_balance_base(const Address &addr)
{
  auto *lock = addr_balance_locks_.get(addr);
  if (!lock) {
    return db_.read_address_balance(addr);
  }
  auto base = lock->get_cached_nnint128_base();
  return base.has_value() ? *base : 0;
}

HogDBStatus<void>
BlockTX::commit(WorkerPool &pool)
{
  std::mutex lock;
  HogDBStatus<void> ret;

  PerThread<KeyChangeBatch> key_changes;

  // write item changes.
  item_locks_.for_each_parallel_destructive(
      [this, &lock, &ret,
       &key_changes](std::pair<const ItemKey &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        HogDBStatus<void> tmp = v.second.write_committed_change(
            LockContext(&db_, block_height_, v.first, &cost_model_));
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }

        if (v.first.is_address_auth_data()) {
          if (auto const *inner = v.second.get_inner<PointerLock<AuthLock>>()) {
            AddressAuthData auth_data = inner->get()->build_result();
            key_changes->emplace_back(v.first.to_contract_address(),
                                      x23::Ptr<AddressAuthData>(
                                          new AddressAuthData(
                                              inner->get()->build_result())));
          }
          else if (v.second.get_inner<DeletionLock>()) {
            key_changes->emplace_back(v.first.to_contract_address(), nullptr);
          }
          else {
            panic("unsupported type at auth data key: {}", v.first);
          }
        }
      },
      1024, pool);

  PerThread<std::vector<Delta>> balance_deltas;

  // write address balances.
  addr_balance_locks_.for_each_parallel_destructive(
      [this, &lock, &ret,
       &balance_deltas](std::pair<const Address &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }

        HogDBStatus<void> tmp = v.second.write_committed_change(
            LockContext(&db_, block_height_,
                        make_address_balance_item_key(v.first), &cost_model_));
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }

        auto const *inner = v.second.get_inner<BalanceLock>();
        CHECK(inner != nullptr);

        auto value = inner->get_delta();

        balance_deltas->push_back(
            Delta{.address = v.first.get_raw(), .hog_delta = to_xdr(value)});
      },
      1024, pool);

  if (ret) {
    background_commit_worker_.submit_job(block_height_, std::move(key_changes),
                                         std::move(balance_deltas));
  }

  return ret;
}

DbHash
BlockTX::compute_close_hash(const DbHash &base_hash)
{
  LtHash16 hash = lthash16_from_db_hash(base_hash);
  std::mutex hash_lock;

  item_locks_.for_each_parallel([&](std::pair<const ItemKey &, ItemLock &> v) {
    if (!v.second.is_finalized()) {
      return;
    }

    const ItemKey &key = v.first;

    if (auto const *inner = v.second.get_inner<NNInt128Lock>()) {
      auto [base, delta] = inner->get_base_and_delta();
      int128_t out = cpu_to_le(base + delta);
      auto raw_key = make_contract_data_db_key(key.as_bytes());
      std::scoped_lock g(hash_lock);
      replace_known_item(hash, db_, raw_key, kNNInt128, byte_view(out));
      return;
    }

    if (auto const *inner = v.second.get_inner<BytestringLock>()) {
      (void)inner;
      auto raw_key = make_contract_data_db_key(key.as_bytes());
      auto raw = db_.read_bytes(key);
      std::scoped_lock g(hash_lock);
      if (raw.has_value()) {
        lthash_remove_item(hash, raw_key, kBytes, *raw);
      }
      // We don't currently expose the lock's byte payload; hash a stable
      // marker.
      x23::Vec<std::byte> marker;
      marker.push_back(std::byte{0x42});
      lthash_add_item(hash, raw_key, kBytes, marker);
      return;
    }

    if (auto const *inner = v.second.get_inner<PointerLock<AuthLock>>()) {
      auto auth = inner->get()->build_result();
      auto auth_key = make_address_key_with_prefix(key.get_address(),
                                                   Prefix::kAddressAuthData);
      auto serialized = x23::serialize(auth);
      std::scoped_lock g(hash_lock);
      replace_known_item(hash, db_, auth_key, 0,
                         std::as_bytes(std::span(serialized)));
      return;
    }

    if (auto const *inner = v.second.get_inner<NewDeploymentLock>()) {
      (void)inner;
      auto meta = db_.read_address_metadata(key.get_address());
      auto meta_key = make_address_key_with_prefix(key.get_address(),
                                                   Prefix::kAddressMetadata);
      std::scoped_lock g(hash_lock);
      if (meta.has_value()) {
        auto old = x23::serialize(*meta);
        lthash_remove_item(hash, std::as_bytes(std::span(meta_key)), 0,
                           std::as_bytes(std::span(old)));
      }
      auto marker = x23::serialize(key.get_address().get_raw());
      lthash_add_item(hash, std::as_bytes(std::span(meta_key)), 0,
                      std::as_bytes(std::span(marker)));
      return;
    }

    if (auto const *inner = v.second.get_inner<NewContractLock>()) {
      (void)inner;
      SHA256Hash code_hash = key.get_contract_code_hash();
      std::array<std::byte, std::tuple_size_v<SHA256Hash> + 1> wasm_key{};
      wasm_key[0] = static_cast<std::byte>(Prefix::kWasmCode);
      std::ranges::copy(std::as_bytes(std::span(code_hash)),
                        std::begin(std::span(wasm_key)) + 1);
      auto wasm = db_.read_wasm_code(code_hash);
      std::scoped_lock g(hash_lock);
      if (wasm.has_value()) {
        lthash_remove_item(hash, std::as_bytes(std::span(wasm_key)), 0, *wasm);
      }
      auto marker = x23::serialize(code_hash);
      lthash_add_item(hash, std::as_bytes(std::span(wasm_key)), 0,
                      std::as_bytes(std::span(marker)));
      return;
    }

    if (v.second.get_inner<DeletionLock>()) {
      auto raw_key = make_contract_data_db_key(key.as_bytes());
      auto before = db_.get_ramdb().get(raw_key);
      std::scoped_lock g(hash_lock);
      if (before.has_value()) {
        lthash_remove_item(hash, raw_key, before->type, before->value);
      }
      return;
    }
  });

  addr_balance_locks_.for_each_parallel(
      [&](std::pair<const Address &, ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        auto const *inner = v.second.get_inner<BalanceLock>();
        if (!inner) {
          return;
        }
        auto balance_key =
            make_address_key_with_prefix(v.first, Prefix::kBalance);
        auto before = db_.read_address_balance(v.first);
        std::scoped_lock g(hash_lock);
        if (before.has_value()) {
          int128_t prev = cpu_to_le(*before);
          lthash_remove_item(hash, std::as_bytes(std::span(balance_key)),
                             kNNInt128, byte_view(prev));
        }
        int128_t after = before.value_or(0) + inner->get_delta();
        int128_t encoded_after = cpu_to_le(after);
        lthash_add_item(hash, std::as_bytes(std::span(balance_key)), kNNInt128,
                        byte_view(encoded_after));
      });

  return db_hash_from_lthash16(hash);
}

} // namespace hogd
