// hogdb_dao.h - provides a data access object (DAO) for hogdb's persistent
// state
//
// It abstracts away the low-level details of storing objects in the ramdb.
//
// TODO(abelay): We need to add support for account deletion, which should
// probably delete all keys associated with an account.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

// xdr types
#include "address_auth.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/invariant.h"
#include "hogd/hogdb/invariant_hog_age.h"
#include "hogd/hogdb/invariant_hog_conservation.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/serialize.h"

namespace hogd {

class ChangeSet;

// RAMDB is the low-level interface used by hogdb to access the ramdb.
class RAMDB {
public:
  RAMDB() = default;
  explicit RAMDB(std::unique_ptr<Database> db) : db_(std::move(db)) {}
  ~RAMDB() = default;

  RAMDB(RAMDB &&db) noexcept : db_(std::move(db.db_)) {}
  RAMDB &operator=(RAMDB &&db) noexcept
  {
    db_ = std::move(db.db_);
    return *this;
  }
  RAMDB(const RAMDB &db) = delete;
  RAMDB &operator=(const RAMDB &db) = delete;

  HogDBStatus<DBItem> get(std::span<const std::byte> key)
  {
    return db_->get(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<void> get_if_type(std::span<const std::byte> key, uint8_t type,
                                std::span<std::byte> val_out)
  {
    return db_->get_if_type(key, type, val_out).transform_error(to_hogdb_error);
  }

  HogDBStatus<OwningSpan<const std::byte>>
  get_if_type(std::span<const std::byte> key, uint8_t type)
  {
    return db_->get_if_type(key, type).transform_error(to_hogdb_error);
  }

  HogDBStatus<size_t> get_size(std::span<const std::byte> key)
  {
    return db_->get_size(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<uint8_t> get_type(std::span<const std::byte> key)
  {
    return db_->get_type(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                        std::span<const std::byte> val)
  {
    return db_->put(key, type, val).transform_error(to_hogdb_error);
  }

  HogDBStatus<void> remove(std::span<const std::byte> key)
  {
    return db_->remove(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<uint64_t> commit()
  {
    return db_->sync().transform_error(to_hogdb_error);
  }

  // disk_bytes_written returns the total bytes written to WAL log files since
  // the last database recovery.
  size_t disk_bytes_written() const { return db_->disk_bytes_written(); }

  void get_each_parallel(GetEachCallback fn,
                         size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    db_->get_each_parallel(std::move(fn), width, pool);
  }

  // Layout and compaction introspection, for benchmarks that measure where
  // items physically land rather than what they contain.
  void for_each_location(LayoutCallback fn)
  {
    db_->for_each_location(std::move(fn));
  }
  size_t loglet_count() const { return db_->loglet_count(); }
  void enable_heat_tracking(int width_order = 20)
  {
    db_->enable_heat_tracking(width_order);
  }
  void age_heat() { db_->age_heat(); }
  size_t heat_bytes() const { return db_->heat_bytes(); }
  uint8_t heat_estimate(std::span<const std::byte> key) const
  {
    return db_->heat_estimate(key);
  }
  uint8_t heat_threshold(double hot_fraction) const
  {
    return db_->heat_threshold(hot_fraction);
  }
  bool heat_enabled() const { return db_->heat_enabled(); }
  void record_heat(std::span<const std::byte> key) { db_->record_heat(key); }
  size_t consolidate(const std::function<bool(ItemHeader *)> &is_hot,
                     float min_mixedness, size_t max_victims = 0)
  {
    return db_->consolidate(is_hot, min_mixedness, max_victims);
  }
  void enable_auto_consolidation(double hot_fraction, size_t max_victims,
                                 float min_mixedness = 0.0F)
  {
    db_->enable_auto_consolidation(hot_fraction, max_victims, min_mixedness);
  }
  size_t consolidate_incremental() { return db_->consolidate_incremental(); }
  uint64_t consolidation_passes() const { return db_->consolidation_passes(); }
  uint64_t consolidation_loglets() const
  {
    return db_->consolidation_loglets();
  }
  uint64_t compaction_passes() const { return db_->compaction_passes(); }
  uint64_t compaction_bytes_reclaimed() const
  {
    return db_->compaction_bytes_reclaimed();
  }

private:
  std::unique_ptr<Database> db_;
};

// Prefix is a prefix appended to each key to form an overall key that
// indicates the category of data being stored. The rest of the key is normally
// a contract address, except for kWasmCode, which uses the hash of the wasm
// blob as the rest of its key.
//
// Warning: These key prefixes are persisted on disk so they cannot be changed
// without migrating existing data.
//
// Not every prefix type is included in the db hash: state-item prefixes (see
// is_state_item) are folded into the consensus StorageLongHash, while kHogAge
// and kGlobalMetadata are not.
//
// Because these bytes are persisted verbatim as the first byte of each key --
// and, for state items, hashed into consensus -- they must be fixed protocol
// bytes. They use `u8'...'` character literals, whose value is the
// standard-mandated UTF-8 code unit (== ASCII here) regardless of the ordinary
// execution character set (`-fexec-charset`). An ordinary `'B'` would instead
// take an implementation-defined value, so a build with e.g. an EBCDIC
// execution charset would persist different bytes and hash differently. See
// hogd issue #2600.
enum class Prefix : uint8_t {
  kHogAge = u8'A',
  kBalance = u8'B',
  kAddressAuthData = u8'D',
  kAddressMetadata = u8'M',
  kContractDataKey = u8'K',
  kWasmCode = u8'C',
  kWasmCodeRefcount = u8'R',
  kReplayCache = u8'U',
  kRetrocausalQueueBlock = u8'Q',
  // global info, like the db tip, have prefixes that begin with (and may
  // extend) this byte
  kGlobalMetadata = u8'<',
};

// Warning: you may have to update this if you make changes to Prefix.
constexpr bool
is_state_item(std::span<const std::byte> key)
{
  if (key.empty()) {
    return false;
  }

  auto prefix = static_cast<Prefix>(key[0]);
  switch (prefix) {
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
    error("unknown prefix: {}", std::to_underlying(prefix));
    return false;
  }
}

constexpr auto
make_address_key(const Address &addr, Prefix p)
{
  std::array<std::byte, kAddressSize + 1> ret;
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1);
  ret[0] = static_cast<std::byte>(p);
  return ret;
}

constexpr auto
make_wasm_code_key(const SHA256Hash &hash)
{
  std::array<std::byte, hash.size() + 1> ret;
  std::ranges::copy(std::as_bytes(std::span(hash)),
                    std::begin(std::span(ret)) + 1);
  ret[0] = static_cast<std::byte>(Prefix::kWasmCode);
  return ret;
}

// Item type identifiers; stored in the type field in ramdb. This is the one
// enum for item type bytes: values must never overlap, even though some are
// only meaningful under particular key prefixes. Every value is
// consensus-frozen once shipped -- the (key, type, value) triple of each
// state item feeds the db_hash.
//
// kNNInt128/kBytes/kRetrocausalQueue are used where multiple types can be
// stored at an address (ItemKeys; address balances are also marked as
// kNNInt128 since hogdb treats them like a special ItemKey for native
// tokens).
//
// The kWasmCode* types mark Prefix::kWasmCode rows. A code row whose
// serialized Contract fits one RAMDB item is stored inline (kWasmCodeInline)
// -- the exact layout that predates chunking, so existing state and its
// db_hash contribution are unchanged. A larger row is stored as a head item
// (same 33-byte key, type kWasmCodeChunkedHead, value = XDR uint32 total
// length) plus payload chunks at 34-byte keys (prefix || hash || chunk
// index), type kWasmCodeChunk. The layout is a pure function of the
// serialized size, so every node produces identical items (and identical
// db_hash deltas).
enum : uint8_t {
  // Predates chunking (and the other type identifiers); must stay 0.
  kWasmCodeInline = 0,
  kNNInt128 = 1,
  kBytes = 2,
  kRetrocausalQueue = 3,
  // Chunked wasm code rows (#2569).
  kWasmCodeChunkedHead = 4,
  kWasmCodeChunk = 5,
};

// Payload bytes per chunk item, sized so every chunk satisfies item_fits for
// a 34-byte chunk key. This keeps the RAMDB item ceiling private to
// ramdb/hogdb: callers above the DAO never see it.
inline constexpr size_t kWasmCodeChunkPayloadSize = 63_KiB;
static_assert(Database::item_fits(sizeof(SHA256Hash) + 2,
                                  kWasmCodeChunkPayloadSize));

constexpr auto
make_wasm_code_chunk_key(const SHA256Hash &hash, uint8_t chunk_index)
{
  std::array<std::byte, hash.size() + 2> ret;
  ret[0] = static_cast<std::byte>(Prefix::kWasmCode);
  std::ranges::copy(std::as_bytes(std::span(hash)),
                    std::begin(std::span(ret)) + 1);
  ret[hash.size() + 1] = static_cast<std::byte>(chunk_index);
  return ret;
}

// Emits the RAMDB items that store one wasm code row, in write order, as
// put(key_span, type, value_span) calls. Chunks are emitted before the
// chunked head so a reader that finds a head always finds complete chunks
// (code rows are content-addressed and never modified or deleted). Shared by
// HogDBDAO::write_wasm_code and NewContractLock::stage_committed_changes so
// the two write paths cannot diverge.
template<typename PutFn>
void
for_each_wasm_code_item(const SHA256Hash &hash, std::span<const std::byte> buf,
                        PutFn &&put)
{
  auto head_key = make_wasm_code_key(hash);
  auto head_key_span = std::as_bytes(std::span(head_key));
  if (Database::item_fits(head_key.size(), buf.size())) {
    put(head_key_span, kWasmCodeInline, buf);
    return;
  }

  const size_t chunk_count =
      (buf.size() + kWasmCodeChunkPayloadSize - 1) / kWasmCodeChunkPayloadSize;
  // One-byte chunk indexes. InvokeTX::request_new_contract bounds production
  // rows far below this; only a buggy direct caller can exceed it.
  CHECK(chunk_count <= 255);

  for (size_t i = 0; i < chunk_count; ++i) {
    auto chunk_key = make_wasm_code_chunk_key(hash, static_cast<uint8_t>(i));
    const size_t off = i * kWasmCodeChunkPayloadSize;
    put(std::as_bytes(std::span(chunk_key)), kWasmCodeChunk,
        buf.subspan(off,
                    std::min(kWasmCodeChunkPayloadSize, buf.size() - off)));
  }

  auto total_len = x23::serialize(static_cast<uint32_t>(buf.size()));
  put(head_key_span, kWasmCodeChunkedHead, std::as_bytes(std::span(total_len)));
}

constexpr auto
make_contract_data_key(std::span<const std::byte> key)
{
  x23::Vec<std::byte> buf;
  buf.resize_uninitialized(key.size() + 1);
  buf[0] = static_cast<std::byte>(Prefix::kContractDataKey);
  std::ranges::copy(key, std::begin(std::span(buf)) + 1);
  return buf;
}

constexpr size_t kContractDataItemKeySize = 1 + kAddressSize + kItemKeySize;

constexpr auto
make_contract_data_key(const ItemKey &key)
{
  assert(key.is_contract_data());
  auto key_bytes = key.as_bytes();
  assert(key_bytes.size() == kContractDataItemKeySize - 1);
  std::array<std::byte, kContractDataItemKeySize> ret;
  ret[0] = static_cast<std::byte>(Prefix::kContractDataKey);
  std::ranges::copy(key_bytes, std::begin(ret) + 1);
  return ret;
}

// ItemLockCacheData is the data we use to cache type-specific info,
// like the nnint base value, so we don't have to load that on each
// request_nnint128.

class ItemLockCacheData {
public:
  static ItemLockCacheData from_balance(std::optional<int128_t> balance)
  {
    return ItemLockCacheData(CachedBalance{balance});
  }

  static ItemLockCacheData from_nnint128_base(int128_t base_value)
  {
    return ItemLockCacheData(CachedNNInt128Base{base_value});
  }

  static ItemLockCacheData from_none() { return ItemLockCacheData(); }
  static ItemLockCacheData from_rcq() { return ItemLockCacheData(CachedRCQ{}); }

  bool is_cached_balance() const
  {
    return std::holds_alternative<CachedBalance>(data_);
  }

  bool is_cached_nnint128_base() const
  {
    return std::holds_alternative<CachedNNInt128Base>(data_);
  }

  bool is_none() const { return std::holds_alternative<std::monostate>(data_); }
  bool is_rcq() const { return std::holds_alternative<CachedRCQ>(data_); }

  std::optional<int128_t> get_cached_balance() const
  {
    assert(std::holds_alternative<CachedBalance>(data_));
    return std::get<CachedBalance>(data_).balance;
  }

  int128_t get_cached_nnint128_base() const
  {
    assert(std::holds_alternative<CachedNNInt128Base>(data_));
    return std::get<CachedNNInt128Base>(data_).base_value;
  }

private:
  struct CachedBalance {
    std::optional<int128_t> balance;
  };

  struct CachedNNInt128Base {
    int128_t base_value;
  };

  // Tag-only: no cached data. RCQ metadata is loaded lazily by
  // RetrocausalQueueLock. The tag's purpose is to let ItemLock detect
  // type changes that require cleanup of RCQ block entries.
  struct CachedRCQ {};

  ItemLockCacheData() : data_(std::monostate{}) {}
  ItemLockCacheData(CachedBalance balance) : data_(balance) {}
  ItemLockCacheData(CachedNNInt128Base nnint128_base) : data_(nnint128_base) {}
  ItemLockCacheData(CachedRCQ /*tag*/) : data_(CachedRCQ{}) {}

  std::variant<std::monostate, CachedBalance, CachedNNInt128Base, CachedRCQ>
      data_;
};

// create a key for a replay cache data item that holds nonces for with a
// particular expiry block height. Exposed so that ReplayCacheLock can use it
// to formulate correct keys to sub-items that are part of each replay cache.
constexpr auto
make_replay_cache_data_key(const Address &addr, BlockHeight height)
{
  std::array<std::byte, kAddressSize + 1 + sizeof(BlockHeight)> ret;
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1);
  ret[0] = static_cast<std::byte>(Prefix::kReplayCache);

  auto h = cpu_to_le(height);
  std::ranges::copy(std::as_bytes(std::span(&h, 1)),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1 +
                        kAddressSize);
  return ret;
}

constexpr auto
make_retrocausal_queue_block_key(const ItemKey &key, uint64_t block_number)
{
  std::span<const std::byte> key_bytes = key.as_bytes();

  std::vector<std::byte> ret;
  ret.reserve(1 + key_bytes.size() + sizeof(uint64_t));

  ret.push_back(static_cast<std::byte>(Prefix::kRetrocausalQueueBlock));

  std::ranges::copy(key_bytes, std::back_inserter(ret));

  auto n = cpu_to_le(block_number);
  std::ranges::copy(std::as_bytes(std::span(&n, 1)), std::back_inserter(ret));

  return ret;
}

// hogDBDAO provides a data access object (DAO) for hogdb's persistent state.
class HogDBDAO {
public:
  HogDBDAO() = default;
  explicit HogDBDAO(RAMDB &&db) : db_(std::move(db)) {}

  // returns the low-level ramdb (direct access to the DB).
  RAMDB &get_ramdb() { return db_; }

  // Create a ChangeSet for accumulating materialized changes.
  // When compute_hash is true, put()/remove() accumulate LtHash16 deltas
  // inline; when false, hashing is skipped entirely.
  ChangeSet create_changeset(bool compute_hash = true);

  // Apply all materialized changes from a ChangeSet to the database.
  HogDBStatus<void> apply(ChangeSet &changes,
                          WorkerPool &pool = WorkerPool::get_default_pool());

  // commit persists all pending changes in durable storage.
  HogDBStatus<uint64_t> commit() { return db_.commit(); }

  // disk_bytes_written returns the total bytes written to WAL log files since
  // the last database recovery.
  size_t disk_bytes_written() const { return db_.disk_bytes_written(); }

  // Hotness tracking, recorded at the logical access point rather than the
  // storage read path. See Database::record_heat.
  bool heat_enabled() const { return db_.heat_enabled(); }
  void record_heat(std::span<const std::byte> key) { db_.record_heat(key); }

  void enable_debug_invariants(StorageCostModel const &cost_model)
  {
    assert(invariants_.empty());
    invariants_.push_back(
        std::make_unique<HogConservationInvariant>(cost_model));
    invariants_.push_back(std::make_unique<HogAgeInvariant>());
  }

  void clear_invariants() { invariants_.clear(); }

  bool check_invariants()
  {
    bool ret = true;
    for (auto &invariant : invariants_) {
      if (!invariant->check()) {
        ret = false;
        break;
      }
    }
    invariants_.clear();
    return ret;
  }

  // for_each_state_item calls fn in parallel for every item that needs to be
  // included in the db hash. It is typically used at initialization time when
  // the database is first loaded.
  void for_each_state_item(GetEachCallback fn);

  // computes the db hash from scratch.
  StorageLongHash compute_full_db_hash();

  //
  // Global Operations
  //

  HogDBStatus<DbTip> read_dbtip();
  HogDBStatus<void> write_dbtip(const DbTip &tip);

  HogDBStatus<StorageCostModel> read_cost_model();
  HogDBStatus<void> write_cost_model(const StorageCostModel &cost_model);

  //
  // Address Operations
  //

  HogDBStatus<int128_t> read_address_balance(const Address &addr);
  HogDBStatus<HogAge> read_address_average_hog_age(const Address &addr);
  HogDBStatus<AddressAuthData> read_address_auth_data(const Address &addr);
  HogDBStatus<DeployedContractHash> read_address_metadata(const Address &addr);
  HogDBStatus<ReplayCacheMetadata>
  read_address_replay_cache_meta(Address const &addr);
  HogDBStatus<ReplayCacheData>
  read_address_replay_cache_data(Address const &addr, BlockHeight height);

  HogDBStatus<void> write_address_balance(const Address &addr, int128_t val);
  HogDBStatus<void> write_address_average_hog_age(const HogAge &age);
  HogDBStatus<void> write_address_auth_data(const Address &addr,
                                            const AddressAuthData &auth);
  HogDBStatus<void> write_address_metadata(Address const &addr,
                                           const DeployedContractHash &hash);
  HogDBStatus<void>
  write_address_replay_cache_meta(Address const &addr,
                                  ReplayCacheMetadata const &meta);
  HogDBStatus<void>
  write_address_replay_cache_data(Address const &addr, BlockHeight height,
                                  ReplayCacheData const &data);
  HogDBStatus<void> remove_address_replay_cache_data(const Address &addr,
                                                     BlockHeight height);

  HogDBStatus<RetrocausalQueueBlock>
  read_retrocausal_queue_block(const ItemKey &key, uint64_t block_number);
  HogDBStatus<void>
  write_retrocausal_queue_block(const ItemKey &key,
                                const RetrocausalQueueBlock &block);
  HogDBStatus<void> remove_retrocausal_queue_block(const ItemKey &key,
                                                   uint64_t block_number);

  HogDBStatus<RetrocausalQueueMetadata>
  read_retrocausal_queue_metadata(const ItemKey &key);
  HogDBStatus<void>
  write_retrocausal_queue_metadata(const ItemKey &key,
                                   const RetrocausalQueueMetadata &metadata);
  HogDBStatus<void> remove_retrocausal_queue_metadata(const ItemKey &key);

  //
  // WASM Operations
  //

  HogDBStatus<OwningSpan<const std::byte>>
  read_wasm_code(const SHA256Hash &hash);
  HogDBStatus<void> write_wasm_code(const SHA256Hash &hash,
                                    std::span<const std::byte> buf);

  //
  // ItemKey Operations
  //

  HogDBStatus<int128_t> read_nnint128(const ItemKey &key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(const ItemKey &key);
  // read any of the item types
  HogDBStatus<DBItem> read_contract_data_item(const ItemKey &key);

  HogDBStatus<void> write_nnint128(const ItemKey &key, int128_t val);
  HogDBStatus<void> write_bytes(const ItemKey &key,
                                std::span<const std::byte> buf);

  HogDBStatus<std::pair<int128_t, ItemLockCacheData>>
  get_charged_storage_cost_and_cache_data(const ItemKey &key,
                                          StorageCostModel const *cost_model);
  HogDBStatus<int8_t> get_item_type(const ItemKey &key);

  HogDBStatus<void> remove_item(const ItemKey &key);

private:
  std::optional<HogDBStatus<DBItem>>
  get_before_item(std::span<const std::byte> key)
  {
    if (!invariants_.empty()) {
      return db_.get(key);
    }
    return std::nullopt;
  }

  void update_invariants_post(std::span<const std::byte> key,
                              std::optional<HogDBStatus<DBItem>> const &before)
  {
    if (!invariants_.empty()) {
      assert(before.has_value());
      auto after = db_.get(key);
      for (auto &invariant : invariants_) {
        invariant->log_item_change(key, *before, after);
      }
    }
  }

  std::vector<std::unique_ptr<HogdbInvariant>> invariants_;
  RAMDB db_;
};

} // namespace hogd
