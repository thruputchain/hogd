#include "hogd/hogdb/hogdb_dao.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// xdr types
#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/bits.h"
#include "hogd/base/perthread.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/materialized_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr std::string kGlobalTipKey = "<DBTIP";
constexpr std::string kGlobalCostModelKey = "<COSTMODEL";

} // namespace

DbHash
HogDBDAO::compute_full_db_hash()
{
  PerThread<LtHash16> per_thread_hash;
  for_each_state_item([&per_thread_hash](std::span<const std::byte> key,
                                         uint8_t type,
                                         std::span<const std::byte> value) {
    lthash_add_item(*per_thread_hash, key, type, value);
  });

  LtHash16 db_hash;
  auto ul = per_thread_hash.lock_threads();
  per_thread_hash.for_each(ul, [&db_hash](const LtHash16 &hash) {
    db_hash += hash;
  });
  return db_hash_from_lthash16(db_hash);
}

void
HogDBDAO::for_each_state_item(GetEachCallback fn)
{
  db_.get_each_parallel([&fn](std::span<const std::byte> key, uint8_t type,
                              std::span<const std::byte> value) {
    if (is_state_item(key))
      fn(key, type, value);
  });
}

HogDBStatus<DbTip>
HogDBDAO::read_dbtip()
{
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalTipKey)), 0);
  if (!raw)
    return raw.error();

  DbTip tip;
  try {
    x23::deserialize(&tip, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return tip;
}

HogDBStatus<void>
HogDBDAO::write_dbtip(const DbTip &tip)
{
  auto buf = x23::serialize(tip);
  return db_.put(std::as_bytes(std::span(kGlobalTipKey)), 0,
                 std::as_bytes(std::span(buf)));
}

HogDBStatus<void>
HogDBDAO::write_cost_model(const StorageCostModel &cost_model)
{
  auto buf = x23::serialize(cost_model.to_xdr());
  auto key = std::as_bytes(std::span(kGlobalCostModelKey));
  if (db_.get(key)) {
    error("Multiple versions or changing cost models not supported yet");
    return HogDBError(HogDBCode::kInternal);
  }
  return db_.put(key, 0, std::as_bytes(std::span(buf)));
}

HogDBStatus<StorageCostModel>
HogDBDAO::read_cost_model()
{
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalCostModelKey)), 0);
  if (!raw)
    return raw.error();
  StorageCostModelXdr cost_model{};
  try {
    x23::deserialize(&cost_model, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return StorageCostModel(cost_model);
}

HogDBStatus<int128_t>
HogDBDAO::read_address_balance(const Address &addr)
{
  int128_t val;
  auto key = make_address_key(addr, Prefix::kBalance);
  HogDBStatus<void> ret = db_.get_if_type(std::as_bytes(std::span(key)),
                                          kNNInt128, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

HogDBStatus<HogAge>
HogDBDAO::read_address_average_hog_age(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kHogAge);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  HogAge age;
  try {
    x23::deserialize(&age, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return age;
}

HogDBStatus<ReplayCacheMetadata>
HogDBDAO::read_address_replay_cache_meta(Address const &addr)
{
  auto key = make_address_key(addr, Prefix::kReplayCache);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  ReplayCacheMetadata meta;
  try {
    x23::deserialize(&meta, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return meta;
}

HogDBStatus<ReplayCacheData>
HogDBDAO::read_address_replay_cache_data(Address const &addr,
                                         BlockHeight height)
{
  auto key = make_replay_cache_data_key(addr, height);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  ReplayCacheData data;
  try {
    x23::deserialize(&data, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return data;
}

// TODO(jhumphri): Handle [block_id].
HogDBStatus<AddressAuthData>
HogDBDAO::read_address_auth_data(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kAddressAuthData);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  AddressAuthData auth;
  try {
    x23::deserialize(&auth, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return auth;
}

HogDBStatus<DeployedContractHash>
HogDBDAO::read_address_metadata(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kAddressMetadata);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);

  if (!raw)
    return raw.error();

  DeployedContractHash deployed_contract_hash;
  try {
    x23::deserialize(&deployed_contract_hash, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return deployed_contract_hash;
}

HogDBStatus<OwningSpan<const std::byte>>
HogDBDAO::read_wasm_code(const SHA256Hash &hash)
{
  auto key = make_wasm_code_key(hash);
  return db_.get_if_type(std::as_bytes(std::span(key)), 0);
}

HogDBStatus<int128_t>
HogDBDAO::read_nnint128(const ItemKey &key)
{
  if (key.is_address_balance())
    return read_address_balance(key.get_address());

  int128_t val;
  HogDBStatus<void> ret =
      db_.get_if_type(make_contract_data_key(key.as_bytes()), kNNInt128,
                      writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

HogDBStatus<OwningSpan<const std::byte>>
HogDBDAO::read_bytes(const ItemKey &key)
{
  return db_.get_if_type(make_contract_data_key(key.as_bytes()), kBytes);
}

HogDBStatus<void>
HogDBDAO::write_address_balance(const Address &addr, int128_t val)
{
  auto key = make_address_key(addr, Prefix::kBalance);
  auto before_item = get_before_item(key);
  auto ret = db_.put(std::as_bytes(std::span(key)), kNNInt128,
                     byte_view(cpu_to_le(val)));

  update_invariants_post(key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_average_hog_age(const HogAge &age)
{
  auto key = make_address_key(age.address, Prefix::kHogAge);
  auto before_item = get_before_item(key);
  auto buf = x23::serialize(age);
  auto ret =
      db_.put(std::as_bytes(std::span(key)), 0, std::as_bytes(std::span(buf)));
  update_invariants_post(key, before_item);
  return ret;
}

// TODO(jhumphri): Handle [block_id].
HogDBStatus<void>
HogDBDAO::write_address_auth_data(const Address &addr,
                                  const AddressAuthData &auth)
{
  auto key = make_address_key(addr, Prefix::kAddressAuthData);
  auto buf = x23::serialize(auth);

  auto before_item = get_before_item(key);

  auto ret =
      db_.put(std::as_bytes(std::span(key)), 0, std::as_bytes(std::span(buf)));
  update_invariants_post(key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_replay_cache_meta(Address const &addr,
                                          ReplayCacheMetadata const &meta)
{
  auto key = make_address_key(addr, Prefix::kReplayCache);
  auto buf = x23::serialize(meta);

  auto before_item = get_before_item(key);

  auto ret =
      db_.put(std::as_bytes(std::span(key)), 0, std::as_bytes(std::span(buf)));
  update_invariants_post(key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_replay_cache_data(Address const &addr,
                                          BlockHeight height,
                                          ReplayCacheData const &data)
{
  auto key = make_replay_cache_data_key(addr, height);
  auto buf = x23::serialize(data);
  auto key_span = std::as_bytes(std::span(key));

  auto before_item = get_before_item(key_span);

  auto ret = db_.put(key_span, 0, std::as_bytes(std::span(buf)));
  update_invariants_post(key_span, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::remove_address_replay_cache_data(const Address &addr,
                                           BlockHeight height)
{
  auto key = make_replay_cache_data_key(addr, height);
  auto key_span = std::as_bytes(std::span(key));

  auto before_item = get_before_item(key_span);

  auto ret = db_.remove(key_span);
  update_invariants_post(key_span, before_item);
  return ret;
}

HogDBStatus<RetrocausalQueueBlock>
HogDBDAO::read_retrocausal_queue_block(const ItemKey &key,
                                       uint64_t block_number)
{
  auto block_key = make_retrocausal_queue_block_key(key, block_number);
  auto raw = db_.get_if_type(std::as_bytes(std::span(block_key)), 0);
  if (!raw)
    return raw.error();

  RetrocausalQueueBlock block;
  try {
    x23::deserialize(&block, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return block;
}

HogDBStatus<void>
HogDBDAO::write_retrocausal_queue_block(const ItemKey &key,
                                        const RetrocausalQueueBlock &block)
{
  auto block_key = make_retrocausal_queue_block_key(key, block.block_number);

  auto buf = x23::serialize(block);

  auto key_span = std::as_bytes(std::span(block_key));
  auto ret = db_.put(key_span, 0, std::as_bytes(std::span(buf)));

  // We don't update HogDB invariants for retrocausal queue blocks.
  // This is because we *do* when the metadata is updated.
  return ret;
}

HogDBStatus<void>
HogDBDAO::remove_retrocausal_queue_block(const ItemKey &key,
                                         uint64_t block_number)
{
  auto block_key = make_retrocausal_queue_block_key(key, block_number);

  auto key_span = std::as_bytes(std::span(block_key));
  auto ret = db_.remove(key_span);

  // We don't update HogDB invariants for retrocausal queue blocks.
  // This is because we *do* when the metadata is updated.
  return ret;
}

HogDBStatus<RetrocausalQueueMetadata>
HogDBDAO::read_retrocausal_queue_metadata(const ItemKey &key)
{
  auto db_key = make_contract_data_key(key.as_bytes());
  auto raw = db_.get_if_type(db_key, kRetrocausalQueue);
  if (!raw)
    return raw.error();

  RetrocausalQueueMetadata metadata;
  try {
    x23::deserialize(&metadata, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return metadata;
}

HogDBStatus<void>
HogDBDAO::write_retrocausal_queue_metadata(
    const ItemKey &key, const RetrocausalQueueMetadata &metadata)
{
  auto db_key = make_contract_data_key(key.as_bytes());
  auto buf = x23::serialize(metadata);

  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kRetrocausalQueue, std::as_bytes(std::span(buf)));

  update_invariants_post(db_key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::remove_retrocausal_queue_metadata(const ItemKey &key)
{
  auto db_key = make_contract_data_key(key.as_bytes());
  auto before_item = get_before_item(db_key);
  auto ret = db_.remove(db_key);

  update_invariants_post(db_key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_metadata(Address const &addr,
                                 const DeployedContractHash &hash)
{
  auto key = make_address_key(addr, Prefix::kAddressMetadata);
  auto buf = x23::serialize(hash);
  auto key_span = std::as_bytes(std::span(key));

  auto before_item = get_before_item(key_span);
  auto ret = db_.put(key_span, 0, std::as_bytes(std::span(buf)));

  update_invariants_post(key_span, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_wasm_code(const SHA256Hash &hash,
                          std::span<const std::byte> buf)
{
  auto key = make_wasm_code_key(hash);
  return db_.put(std::as_bytes(std::span(key)), 0, buf);
}

HogDBStatus<void>
HogDBDAO::write_nnint128(const ItemKey &key, int128_t val)
{
  if (key.is_address_balance())
    return write_address_balance(key.get_address(), val);
  assert(key.is_contract_data());

  auto db_key = make_contract_data_key(key.as_bytes());
  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kNNInt128, byte_view(cpu_to_le(val)));
  update_invariants_post(db_key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_bytes(const ItemKey &key, std::span<const std::byte> buf)
{
  auto db_key = make_contract_data_key(key.as_bytes());
  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kBytes, buf);
  update_invariants_post(db_key, before_item);
  return ret;
}

HogDBStatus<std::pair<int128_t, ItemLockCacheData>>
HogDBDAO::get_charged_storage_cost_and_cache_data(
    const ItemKey &key, StorageCostModel const *cost_model)
{
  switch (key.key_type()) {
  case ItemKeyType::kItemKeyTypeFreeNativeTokenBalance:
    assert(key.is_address_balance());
    {
      // this is not charged via the regular storage cost mechanism
      auto r = read_address_balance(key.get_address());
      if (!r && r.error() != HogDBCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDBCode::kNotFound);
        return std::make_pair(0, ItemLockCacheData::from_balance(std::nullopt));
      }
      return std::make_pair(0, ItemLockCacheData::from_balance(r.value()));
    }

  case ItemKeyType::kItemKeyTypeAddressAuthData:
    assert(key.is_address_auth_data());
    {
      auto r = read_address_auth_data(key.get_address());
      if (!r && r.error() != HogDBCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDBCode::kNotFound);
        return HogDBError(HogDBCode::kNotFound);
      }
      size_t active_keys = 0;
      for (size_t i = 0; i < kMaxSigners; ++i) {
        if (r.value().public_keys[i]) {
          active_keys++;
        }
      }
      return std::make_pair(cost_model->get_auth_data_cost(active_keys),
                            ItemLockCacheData::from_none());
    }
  case ItemKeyType::kItemKeyTypeReplayCache:
    assert(key.is_replay_cache());
    {
      auto r = read_address_replay_cache_meta(key.get_address());
      if (!r && r.error() != HogDBCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDBCode::kNotFound);
        return HogDBError(HogDBCode::kNotFound);
      }
      return std::make_pair(cost_model->get_replay_cache_cost(r->size),
                            ItemLockCacheData::from_none());
    }
  case ItemKeyType::kItemKeyTypeAddressMetadata:
    assert(key.is_address_metadata());
    {
      auto db_key =
          make_address_key(key.get_address(), Prefix::kAddressMetadata);
      auto item = db_.get(db_key);
      if (!item) {
        return HogDBError(HogDBCode::kNotFound);
      }
      DeployedContractHash deployed_contract_hash;
      try {
        x23::deserialize(&deployed_contract_hash,
                         x23::BufViewEnd(item.value().value));
      } catch (x23::XdrException &e) {
        error("failed to deserialize deployed contract hash: {}", e.what());
        return HogDBError(HogDBCode::kInternal);
      }
      return std::make_pair(cost_model->get_deployment_cost(
                                deployed_contract_hash.contract_size),
                            ItemLockCacheData::from_none());
    }
  case ItemKeyType::kItemKeyTypeContractData:
  {
    auto item = db_.get(make_contract_data_key(key.as_bytes()));
    if (!item) {
      if (item.error() != HogDBCode::kNotFound) {
        return item.error();
      }
      return HogDBError(HogDBCode::kNotFound);
    }
    switch (item.value().type) {
    case kNNInt128:
    {
      int128_t base_value;
      CHECK(item.value().value.size() == sizeof(int128_t));
      std::memcpy(&base_value, item.value().value.data(), sizeof(int128_t));
      int128_t out = le_to_cpu(base_value);

      return std::make_pair(cost_model->get_nnint_cost(),
                            ItemLockCacheData::from_nnint128_base(out));
    }
    case kBytes:
      return std::make_pair(cost_model->get_bytestring_cost(
                                item.value().value.size()),
                            ItemLockCacheData::from_none());
    case kRetrocausalQueue:
    {
      RetrocausalQueueMetadata meta;
      try {
        x23::deserialize(&meta, x23::BufViewEnd(item.value().value));
      } catch (x23::XdrException &e) {
        error("failed to deserialize rcq metadata: {}", e.what());
        return HogDBError(HogDBCode::kInternal);
      }
      return std::make_pair(cost_model->get_retrocausal_queue_cost(
                                meta.live_entries),
                            ItemLockCacheData::from_rcq());
    }
    default:
      error("unknown item type loaded from db: key={} type={}", key,
            item.value().type);
      return HogDBError(HogDBCode::kInternal);
    }
  }
  case ItemKeyType::kItemKeyTypeContractCode:
  case ItemKeyType::kItemKeyTypeContractRefcount:
    // these keys are not charged: storage costs for contracts are charged on
    // the deployment entries
    return HogDBError(HogDBCode::kNotFound);
  }
  error("unknown item key type: {}", std::to_underlying(key.key_type()));
  return HogDBError(HogDBCode::kInternal);
}

HogDBStatus<int8_t>
HogDBDAO::get_item_type(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDBError(HogDBCode::kInternal);
  }
  return db_.get_type(make_contract_data_key(key.as_bytes()));
}

HogDBStatus<DBItem>
HogDBDAO::read_contract_data_item(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDBError(HogDBCode::kInternal);
  }
  return db_.get(make_contract_data_key(key.as_bytes()));
}

HogDBStatus<void>
HogDBDAO::remove_item(const ItemKey &key)
{
  if (key.is_contract_data()) {
    auto db_key = make_contract_data_key(key.as_bytes());
    auto before_item = get_before_item(db_key);
    auto ret = db_.remove(db_key);
    update_invariants_post(db_key, before_item);
    return ret;
  }
  if (key.is_address_auth_data()) {
    auto db_key = make_address_key(key.get_address(), Prefix::kAddressAuthData);
    auto before_item = get_before_item(db_key);
    auto ret = db_.remove(db_key);
    update_invariants_post(db_key, before_item);
    return ret;
  }
  error("unhandled item key type in remove_item: {}",
        std::to_underlying(key.key_type()));
  return HogDBError(HogDBCode::kInternal);
}

ChangeSet
HogDBDAO::create_changeset(bool compute_hash)
{
  return ChangeSet(this, compute_hash);
}

HogDBStatus<void>
HogDBDAO::apply(ChangeSet &changes, WorkerPool &pool)
{
  // Move per-thread change vectors out of PerThread storage so we own the data.
  // We must not hold raw pointers into PerThread's internal std::vector<T>
  // (TLStorage::items_) after releasing the lock, because a concurrent
  // PerThread<T>::get() on the same thread can trigger items_.resize(), which
  // reallocates the buffer and invalidates all prior pointers/references.
  std::vector<std::vector<MaterializedChange>> vecs;
  {
    auto ul = changes.changes_.lock_threads();
    changes.changes_.for_each(ul,
                              [&vecs](std::vector<MaterializedChange> &vec) {
                                if (!vec.empty()) {
                                  vecs.push_back(std::move(vec));
                                }
                              });
  }

  std::atomic<bool> had_error{false};
  pool.for_each_parallel(
      vecs,
      [this, &had_error](std::vector<MaterializedChange> &vec) {
        for (const auto &change : vec) {
          if (had_error.load(std::memory_order_relaxed)) {
            return;
          }
          switch (change.op) {
          case MaterializedChange::Op::kPut:
            if (auto r = db_.put(change.key(), change.type, change.value());
                !r) {
              had_error.store(true, std::memory_order_relaxed);
              return;
            }
            break;
          case MaterializedChange::Op::kRemove:
            if (auto r = db_.remove(change.key()); !r) {
              had_error.store(true, std::memory_order_relaxed);
              return;
            }
            break;
          }
        }
      },
      1);

  if (had_error.load(std::memory_order_relaxed)) {
    return HogDBError(HogDBCode::kInternal);
  }
  return {};
}

} // namespace hogd
