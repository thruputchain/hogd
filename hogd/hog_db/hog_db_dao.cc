#include "hogd/hog_db/hog_db_dao.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
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
#include "hogd/hog_db/item_lock/retrocausal_queue_lock.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/compiler.h"
#include "hogd/base/perthread.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_db/change_set.h"
#include "hogd/hog_db/db_hash.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/materialized_change.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

static_assert('A' == 0x41 && '0' == 0x30 && '_' == 0x5F,
              "ordinary literal encoding must be ASCII-compatible");

constexpr std::string kGlobalTipKey = "<DBTIP";
constexpr std::string kGlobalCostModelKey = "<COSTMODEL";
constexpr std::string kGlobalShardIdentityKey = "<SHARDID";

} // namespace

StorageLongHash
HogDbDAO::compute_full_db_hash()
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
HogDbDAO::for_each_state_item(GetEachCallback fn)
{
  db_.get_each_parallel([&fn](std::span<const std::byte> key, uint8_t type,
                              std::span<const std::byte> value) {
    if (is_state_item(key))
      fn(key, type, value);
  });
}

HogDbStatus<DbTip>
HogDbDAO::read_dbtip()
{
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalTipKey)), 0);
  if (!raw)
    return raw.error();

  DbTip tip;
  try {
    x23::deserialize(&tip, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return tip;
}

HogDbStatus<void>
HogDbDAO::write_dbtip(const DbTip &tip)
{
  auto buf = x23::serialize(tip);
  return db_.put(std::as_bytes(std::span(kGlobalTipKey)), 0,
                 std::as_bytes(std::span(buf)));
}

HogDbStatus<ShardIdentity>
HogDbDAO::read_shard_identity()
{
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalShardIdentityKey)), 0);
  if (!raw)
    return raw.error();

  ShardIdentity identity;
  try {
    x23::deserialize(&identity, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return identity;
}

HogDbStatus<void>
HogDbDAO::write_shard_identity(const ShardIdentity &identity)
{
  auto buf = x23::serialize(identity);
  return db_.put(std::as_bytes(std::span(kGlobalShardIdentityKey)), 0,
                 std::as_bytes(std::span(buf)));
}

HogDbStatus<void>
HogDbDAO::write_cost_model(const StorageCostModel &cost_model)
{
  auto buf = x23::serialize(cost_model.to_xdr());
  auto key = std::as_bytes(std::span(kGlobalCostModelKey));
  if (db_.get(key)) {
    error("Multiple versions or changing cost models not supported yet");
    return HogDbError(HogDbCode::kInternal);
  }
  return db_.put(key, 0, std::as_bytes(std::span(buf)));
}

HogDbStatus<StorageCostModel>
HogDbDAO::read_cost_model()
{
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalCostModelKey)), 0);
  if (!raw)
    return raw.error();
  StorageCostModelXdr cost_model{};
  try {
    x23::deserialize(&cost_model, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return StorageCostModel(cost_model);
}

HogDbStatus<int128_t>
HogDbDAO::read_address_balance(const Address &addr)
{
  std::array<std::byte, sizeof(int128_xdr)> encoded{};
  auto key = make_address_key(addr, Prefix::kBalance);
  HogDbStatus<void> ret =
      db_.get_if_type(std::as_bytes(std::span(key)), kNNInt128, encoded);
  if (!ret)
    return ret.error();
  return int128_from_xdr_bytes(encoded);
}

HogDbStatus<HogAge>
HogDbDAO::read_address_average_hog_age(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kHogAge);
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  HogAge age;
  try {
    x23::deserialize(&age, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return age;
}

HogDbStatus<ReplayCacheMetadata>
HogDbDAO::read_address_replay_cache_meta(Address const &addr)
{
  auto key = make_address_key(addr, Prefix::kReplayCache);
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  ReplayCacheMetadata meta;
  try {
    x23::deserialize(&meta, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return meta;
}

HogDbStatus<ReplayCacheData>
HogDbDAO::read_address_replay_cache_data(Address const &addr,
                                         BlockHeight height)
{
  auto key = make_replay_cache_data_key(addr, height);
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  ReplayCacheData data;
  try {
    x23::deserialize(&data, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return data;
}

// TODO(jhumphri): Handle [block_id].
HogDbStatus<AddressAuthData>
HogDbDAO::read_address_auth_data(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kAddressAuthData);
  HogDbStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  AddressAuthData auth;
  try {
    x23::deserialize(&auth, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return auth;
}

HogDbStatus<DeployedContractHash>
HogDbDAO::read_address_metadata(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kAddressMetadata);
  constexpr size_t kSize = x23::detail::size_of_fixed<DeployedContractHash>();
  std::array<std::byte, kSize> encoded{};
  HogDbStatus<void> ret =
      db_.get_if_type(std::as_bytes(std::span(key)), 0, encoded);
  if (!ret)
    return ret.error();

  DeployedContractHash deployed_contract_hash;
  try {
    x23::deserialize(&deployed_contract_hash,
                     x23::BufViewEnd(std::span(encoded)));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  return deployed_contract_hash;
}

HogDbStatus<OwningSpan<const std::byte>>
HogDbDAO::read_wasm_code(const SHA256Hash &hash)
{
  auto key = make_wasm_code_key(hash);

  // One lookup covers both layouts: get() returns the type byte and the
  // value together, so the inline path (every pre-chunking row, and the
  // common case) costs a single read.
  auto item = db_.get(std::as_bytes(std::span(key)));
  if (!item)
    return item.error(); // kNotFound passes through unchanged
  if (item->type == kWasmCodeInline)
    return std::move(item->value);
  if (item->type != kWasmCodeChunkedHead)
    return HogDbError(HogDbCode::kCorruption);

  // The chunked head's value is the XDR uint32 total serialized length.
  if (item->value.size() != sizeof(uint32_t))
    return HogDbError(HogDbCode::kCorruption);
  uint32_t total = 0;
  try {
    x23::deserialize(&total, x23::BufViewEnd(std::span(item->value)));
  } catch (x23::XdrException &) {
    return HogDbError(HogDbCode::kCorruption);
  }

  // A chunked head describing a row that would fit inline, or one needing
  // more chunks than a one-byte index addresses, is malformed. Checked
  // before allocating `total` bytes.
  if (Database::item_fits(key.size(), total))
    return HogDbError(HogDbCode::kCorruption);
  const size_t chunk_count =
      (static_cast<size_t>(total) + kWasmCodeChunkPayloadSize - 1) /
      kWasmCodeChunkPayloadSize;
  if (chunk_count > 255)
    return HogDbError(HogDbCode::kCorruption);

  auto *data = new std::byte[total];
  OwningSpan<const std::byte> out(std::span<const std::byte>(data, total),
                                  [data] { delete[] data; });
  for (size_t i = 0; i < chunk_count; ++i) {
    auto chunk_key = make_wasm_code_chunk_key(hash, static_cast<uint8_t>(i));
    const size_t off = i * kWasmCodeChunkPayloadSize;
    const size_t len = std::min(kWasmCodeChunkPayloadSize, total - off);
    // The exact-size get_if_type overload also rejects a wrong-sized chunk.
    if (auto chunk =
            db_.get_if_type(std::as_bytes(std::span(chunk_key)), kWasmCodeChunk,
                            std::span<std::byte>(data + off, len));
        !chunk)
      return HogDbError(HogDbCode::kCorruption);
  }
  return out;
}

HogDbStatus<int128_t>
HogDbDAO::read_nnint128(const ItemKey &key)
{
  if (key.is_address_balance())
    return read_address_balance(key.get_address());

  std::array<std::byte, sizeof(int128_xdr)> encoded{};
  HogDbStatus<void> ret =
      db_.get_if_type(make_contract_data_key(key), kNNInt128, encoded);
  if (!ret)
    return ret.error();
  return int128_from_xdr_bytes(encoded);
}

HogDbStatus<OwningSpan<const std::byte>>
HogDbDAO::read_bytes(const ItemKey &key)
{
  return db_.get_if_type(make_contract_data_key(key), kBytes);
}

HogDbStatus<void>
HogDbDAO::write_address_balance(const Address &addr, int128_t val)
{
  auto key = make_address_key(addr, Prefix::kBalance);
  auto before_item = get_before_item(key);
  auto encoded = int128_to_xdr_bytes(val);
  auto ret = db_.put(std::as_bytes(std::span(key)), kNNInt128, encoded);

  update_invariants_post(key, before_item);
  return ret;
}

HogDbStatus<void>
HogDbDAO::write_address_average_hog_age(const HogAge &age)
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
HogDbStatus<void>
HogDbDAO::write_address_auth_data(const Address &addr,
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

HogDbStatus<void>
HogDbDAO::write_address_replay_cache_meta(Address const &addr,
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

HogDbStatus<void>
HogDbDAO::write_address_replay_cache_data(Address const &addr,
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

HogDbStatus<void>
HogDbDAO::remove_address_replay_cache_data(const Address &addr,
                                           BlockHeight height)
{
  auto key = make_replay_cache_data_key(addr, height);
  auto key_span = std::as_bytes(std::span(key));

  auto before_item = get_before_item(key_span);

  auto ret = db_.remove(key_span);
  update_invariants_post(key_span, before_item);
  return ret;
}

HogDbStatus<RetrocausalQueueBlock>
HogDbDAO::read_retrocausal_queue_block(const ItemKey &key,
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
    return HogDbError(HogDbCode::kInternal);
  }
  return block;
}

HogDbStatus<void>
HogDbDAO::write_retrocausal_queue_block(const ItemKey &key,
                                        const RetrocausalQueueBlock &block)
{
  auto block_key = make_retrocausal_queue_block_key(key, block.block_number);

  auto buf = x23::serialize(block);

  auto key_span = std::as_bytes(std::span(block_key));
  auto ret = db_.put(key_span, 0, std::as_bytes(std::span(buf)));

  // We don't update HogDb invariants for retrocausal queue blocks.
  // This is because we *do* when the metadata is updated.
  return ret;
}

HogDbStatus<void>
HogDbDAO::remove_retrocausal_queue_block(const ItemKey &key,
                                         uint64_t block_number)
{
  auto block_key = make_retrocausal_queue_block_key(key, block_number);

  auto key_span = std::as_bytes(std::span(block_key));
  auto ret = db_.remove(key_span);

  // We don't update HogDb invariants for retrocausal queue blocks.
  // This is because we *do* when the metadata is updated.
  return ret;
}

HogDbStatus<RetrocausalQueueMetadata>
HogDbDAO::read_retrocausal_queue_metadata(const ItemKey &key)
{
  auto db_key = make_contract_data_key(key);
  auto raw = db_.get_if_type(db_key, kRetrocausalQueue);
  if (!raw)
    return raw.error();

  RetrocausalQueueMetadata metadata;
  try {
    x23::deserialize(&metadata, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDbError(HogDbCode::kInternal);
  }
  // live_entries is persisted as an unsigned hyper, but storage-cost
  // accounting is signed 64-bit. RetrocausalQueueLock bans inserts at
  // kMaxRetrocausalQueueLiveEntries, so a value over the hard cap (bound
  // plus one block of overshoot) can only come from on-disk corruption;
  // fail-stop rather than feed it to signed cost arithmetic (issue #2917).
  CHECK_LE(metadata.live_entries, kRetrocausalQueueLiveEntriesHardCap);
  return metadata;
}

HogDbStatus<void>
HogDbDAO::write_retrocausal_queue_metadata(
    const ItemKey &key, const RetrocausalQueueMetadata &metadata)
{
  auto db_key = make_contract_data_key(key);
  auto buf = x23::serialize(metadata);

  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kRetrocausalQueue, std::as_bytes(std::span(buf)));

  update_invariants_post(db_key, before_item);
  return ret;
}

HogDbStatus<void>
HogDbDAO::remove_retrocausal_queue_metadata(const ItemKey &key)
{
  auto db_key = make_contract_data_key(key);
  auto before_item = get_before_item(db_key);
  auto ret = db_.remove(db_key);

  update_invariants_post(db_key, before_item);
  return ret;
}

HogDbStatus<void>
HogDbDAO::write_address_metadata(Address const &addr,
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

HogDbStatus<void>
HogDbDAO::write_wasm_code(const SHA256Hash &hash,
                          std::span<const std::byte> buf)
{
  HogDbStatus<void> ret;
  for_each_wasm_code_item(hash, buf,
                          [this, &ret](std::span<const std::byte> key,
                                       uint8_t type,
                                       std::span<const std::byte> value) {
                            if (ret)
                              ret = db_.put(key, type, value);
                          });
  return ret;
}

HogDbStatus<void>
HogDbDAO::write_nnint128(const ItemKey &key, int128_t val)
{
  if (key.is_address_balance())
    return write_address_balance(key.get_address(), val);
  assert(key.is_contract_data());

  auto db_key = make_contract_data_key(key);
  auto before_item = get_before_item(db_key);
  auto encoded = int128_to_xdr_bytes(val);
  auto ret = db_.put(db_key, kNNInt128, encoded);
  update_invariants_post(db_key, before_item);
  return ret;
}

HogDbStatus<void>
HogDbDAO::write_bytes(const ItemKey &key, std::span<const std::byte> buf)
{
  auto db_key = make_contract_data_key(key);
  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kBytes, buf);
  update_invariants_post(db_key, before_item);
  return ret;
}

HogDbStatus<std::pair<int128_t, ItemLockCacheData>>
HogDbDAO::get_charged_storage_cost_and_cache_data(
    const ItemKey &key, StorageCostModel const *cost_model)
{
  switch (key.key_type()) {
  case ItemKeyType::kItemKeyTypeFreeNativeTokenBalance:
    assert(key.is_address_balance());
    {
      // this is not charged via the regular storage cost mechanism
      auto r = read_address_balance(key.get_address());
      if (!r && r.error() != HogDbCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDbCode::kNotFound);
        return std::make_pair(0, ItemLockCacheData::from_balance(std::nullopt));
      }
      return std::make_pair(0, ItemLockCacheData::from_balance(r.value()));
    }

  case ItemKeyType::kItemKeyTypeAddressAuthData:
    assert(key.is_address_auth_data());
    {
      auto r = read_address_auth_data(key.get_address());
      if (!r && r.error() != HogDbCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDbCode::kNotFound);
        return HogDbError(HogDbCode::kNotFound);
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
      if (!r && r.error() != HogDbCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDbCode::kNotFound);
        return HogDbError(HogDbCode::kNotFound);
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
        return HogDbError(HogDbCode::kNotFound);
      }
      DeployedContractHash deployed_contract_hash;
      try {
        x23::deserialize(&deployed_contract_hash,
                         x23::BufViewEnd(item.value().value));
      } catch (x23::XdrException &e) {
        error("failed to deserialize deployed contract hash: {}", e.what());
        return HogDbError(HogDbCode::kInternal);
      }
      return std::make_pair(cost_model->get_deployment_cost(
                                deployed_contract_hash),
                            ItemLockCacheData::from_none());
    }
  case ItemKeyType::kItemKeyTypeContractData:
  {
    auto item = db_.get(make_contract_data_key(key));
    if (!item) {
      if (item.error() != HogDbCode::kNotFound) {
        return item.error();
      }
      return HogDbError(HogDbCode::kNotFound);
    }
    switch (item.value().type) {
    case kNNInt128:
    {
      CHECK(item.value().value.size() == sizeof(int128_xdr));
      auto encoded = std::span<const std::byte, sizeof(int128_xdr)>(
          item.value().value.data(), sizeof(int128_xdr));
      int128_t out = int128_from_xdr_bytes(encoded);

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
        return HogDbError(HogDbCode::kInternal);
      }
      // See read_retrocausal_queue_metadata: an over-cap live_entries is
      // corrupt on-disk state and must not reach signed cost arithmetic.
      CHECK_LE(meta.live_entries, kRetrocausalQueueLiveEntriesHardCap);
      return std::make_pair(cost_model->get_retrocausal_queue_cost(
                                meta.live_entries),
                            ItemLockCacheData::from_rcq());
    }
    default:
      error("unknown item type loaded from db: key={} type={}", key,
            item.value().type);
      return HogDbError(HogDbCode::kInternal);
    }
  }
  case ItemKeyType::kItemKeyTypeContractCode:
  case ItemKeyType::kItemKeyTypeContractRefcount:
    // these keys are not charged: storage costs for contracts are charged on
    // the deployment entries
    return HogDbError(HogDbCode::kNotFound);
  }
  error("unknown item key type: {}", std::to_underlying(key.key_type()));
  return HogDbError(HogDbCode::kInternal);
}

HogDbStatus<int8_t>
HogDbDAO::get_item_type(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDbError(HogDbCode::kInternal);
  }
  return db_.get_type(make_contract_data_key(key));
}

HogDbStatus<DBItem>
HogDbDAO::read_contract_data_item(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDbError(HogDbCode::kInternal);
  }
  return db_.get(make_contract_data_key(key));
}

HogDbStatus<void>
HogDbDAO::remove_item(const ItemKey &key)
{
  if (key.is_contract_data()) {
    auto db_key = make_contract_data_key(key);
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
  return HogDbError(HogDbCode::kInternal);
}

ChangeSet
HogDbDAO::create_changeset(bool compute_hash)
{
  return ChangeSet(this, compute_hash);
}

HogDbStatus<void>
HogDbDAO::apply(ChangeSet &changes, WorkerPool &pool)
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
        if (had_error.load(std::memory_order_relaxed)) {
          return;
        }
        for (const auto &change : vec) {
          auto key = change.key();

          // Feed the debug invariants the before/after state for every
          // committed item change, mirroring the typed write_* methods. The
          // whole feed is debug-only (invariants are only enabled under
          // is_debug_build()), so it compiles out of release and leaves the
          // parallel apply hot path untouched. Keys are partitioned
          // one-per-lock across the per-thread vectors, so no other worker
          // mutates this key concurrently and the before/after reads are
          // consistent.
          [[maybe_unused]] std::optional<HogDbStatus<DBItem>> before;
          if constexpr (is_debug_build()) {
            before = get_before_item(key);
          }

          switch (change.op) {
          case MaterializedChange::Op::kPut:
            if (auto r = db_.put(key, change.type, change.value()); !r) {
              had_error.store(true, std::memory_order_relaxed);
              return;
            }
            break;
          case MaterializedChange::Op::kRemove:
            if (auto r = db_.remove(key); !r) {
              had_error.store(true, std::memory_order_relaxed);
              return;
            }
            break;
          }

          if constexpr (is_debug_build()) {
            update_invariants_post(key, before);
          }
        }
      },
      1);

  if (had_error.load(std::memory_order_relaxed)) {
    return HogDbError(HogDbCode::kInternal);
  }
  return {};
}

} // namespace hogd
