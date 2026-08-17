#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <rocksdb/compression_type.h>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "event.hh"
#include "event_outbox_internal.hh"
#include "hogd/hog_indexer/status.h"
#include "indexer.hh"
#include "pagination.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

// Forward declare RocksDB types to avoid including headers here
namespace rocksdb {
class DB;
class ColumnFamilyHandle;
class WriteBatch;
} // namespace rocksdb

namespace hogd {

// Custom deleter for RocksDB (defined in .cc file)
struct RocksDBDeleter {
  void operator()(rocksdb::DB *db) const;
};

// RAII wrapper around RocksDB instance for indexer storage
class IndexDB {
public:
  // Create a new database at the specified path
  // Creates the directory if it doesn't exist
  // LZ4 is the suggested default compression, per the rocksdb documentation
  static IndexerStatus<IndexDB>
  make_new(const std::filesystem::path &path,
           rocksdb::CompressionType compression_algorithm =
               rocksdb::CompressionType::kLZ4Compression);

  // Open an existing database at the specified path
  // LZ4 is the suggested default compression, per the rocksdb documentation
  static IndexerStatus<IndexDB>
  open(const std::filesystem::path &path,
       rocksdb::CompressionType compression_algorithm =
           rocksdb::CompressionType::kLZ4Compression);

  static IndexerStatus<IndexDB>
  open_or_make_new(const std::filesystem::path &path,
                   rocksdb::CompressionType compression_algorithm =
                       rocksdb::CompressionType::kLZ4Compression)
  {
    if (std::filesystem::exists(path)) {
      // [path] already exists, so open the existing database.
      return open(path, compression_algorithm);
    }
    // Nothing exists at [path], so create a new database there.
    return make_new(path, compression_algorithm);
  }

  ~IndexDB();

  // Delete copy, allow move
  IndexDB(const IndexDB &) = delete;
  IndexDB &operator=(const IndexDB &) = delete;
  IndexDB(IndexDB &&other) noexcept; // NOLINT(bugprone-exception-escape)
  IndexDB &
  operator=(IndexDB &&other) noexcept; // NOLINT(bugprone-exception-escape)

  static std::string get_build_config();

  // Check if database is open
  bool is_open() const { return db_ != nullptr; }

  static bool
  is_compression_supported(rocksdb::CompressionType compression_algorithm);

  // Consume a batch of events directly
  // Indexes the provided events for the given block_height.
  // [bodies] are the full transaction bodies, index-aligned with [events]
  // (stored in the tx_index value alongside each event log).  May be empty
  // for callers that only exercise the event-log path.
  IndexerStatus<void>
  consume_events(BlockHeight block_height, std::span<const TxEventLog> events,
                 std::span<const ProposedTransaction> bodies);

  // Durable strict ingestion. Epoch changes and committed blocks each have one
  // synchronous RocksDB WriteBatch durability point.
  IndexerProgress durable_progress() const;
  IndexerEpochResult initialize_epoch(const IndexerOutboxOrigin &origin);
  x23::Async<IndexerBlockApplyResult>
  apply_committed_block(IndexerOutboxOrigin origin,
                        IndexerCommittedBlock block);

  // Everything stored in the tx_index for one transaction.
  struct TxRecord {
    BlockHeight height;
    ProposedTransaction tx; // the full transaction body
    TxEventLog events;
  };

  // Look up the full record (height, body, events) for a transaction.
  IndexerStatus<TxRecord> get_tx_record(SHA256Hash const &tx_hash) const;

  // Look up the block height and events for a specific transaction.
  // Returns the block height and TxEventLog if found, or an error if not found.
  IndexerStatus<std::pair<BlockHeight, TxEventLog>>
  get_tx_events(SHA256Hash const &tx_hash) const;

  // Paginated, filtered events query.  See protocol/indexer.x for
  // full semantics.
  //
  // Validates the query (filters non-empty, each filter's
  // contract_addresses non-empty), decodes the cursor (if any),
  // walks the contract_index in the requested direction, applies
  // topic filters, hydrates matches from tx_index, and returns
  // up to `limit` events plus a continuation cursor.
  IndexerStatus<EventPage> get_events(EventQuery const &query) const;

  // Paginated transactions query.  See protocol/indexer.x for full
  // semantics.  The current tx_index is keyed by hash rather than by
  // block height, so this walks the transaction index and sorts the
  // visible rows before paginating.
  IndexerStatus<TransactionPage>
  get_transactions(TransactionQuery const &query) const;

  // Paginated account transaction history: refs to transactions whose ROOT
  // invocation targets `query.address`, in block order.  Reads refs directly
  // off the address index (kind = ROOT_TX); no tx_index hydration.  Clients
  // hydrate full bodies via get_transaction_info.
  IndexerStatus<TransactionRefPage>
  get_transactions_by_address(AddressTransactionQuery const &query) const;

  // Highest block height through which the index is CONTIGUOUS:
  // every block from the first-ever ingested height up to this value
  // is fully ingested.  Out-of-order blocks above a gap are ingested
  // but not served until the gap fills, so everything a query or
  // cursor can observe is immutable.  BlockHeight(0) if nothing has
  // been indexed yet.  Lock-free read, safe concurrently with
  // ingestion.
  BlockHeight latest_block() const;

  // Oldest block height still retained by the indexer.  Today always
  // BlockHeight{0} — retention/pruning is not implemented.  Once it
  // lands, this reflects the lower bound below which queries return
  // empty regardless of anchor.
  static BlockHeight oldest_block() { return BlockHeight{0}; }

  // get_events `limit` clamping.  A request value of 0 means
  // "server default" (kEventsDefaultLimit).  Anything above
  // kEventsMaxLimit is capped to that.
  static constexpr std::uint32_t kEventsDefaultLimit = 50;
  static constexpr std::uint32_t kEventsMaxLimit = 200;
  // Genesis is committed directly at height 0. The durable event/indexer
  // stream begins with the first post-genesis block.
  static constexpr BlockHeight kFirstOutboxBlockHeight = 1;

  void set_legacy_ingest_test_hook(std::function<void()> hook)
  {
    legacy_ingest_test_hook_ = std::move(hook);
  }
  void set_initialize_epoch_test_hook(std::function<void()> hook)
  {
    initialize_epoch_test_hook_ = std::move(hook);
  }
  void set_apply_validation_test_hook(std::function<void()> hook)
  {
    apply_validation_test_hook_ = std::move(hook);
  }

  [[nodiscard]] static constexpr bool
  is_valid_iteration_order(IterationOrder order)
  {
    return order == IterationOrder::IterationOrderAscending ||
           order == IterationOrder::IterationOrderDescending;
  }

  [[nodiscard]] static constexpr std::uint32_t
  clamp_events_limit(std::uint32_t requested)
  {
    if (requested == 0) {
      return kEventsDefaultLimit;
    }
    if (requested > kEventsMaxLimit) {
      return kEventsMaxLimit;
    }
    return requested;
  }

  // Log RocksDB statistics (memory usage, database size, etc.)
  void log_stats() const;

  void launch_stats_worker();

private:
  IndexDB() = default;
  IndexDB(rocksdb::DB *db, rocksdb::ColumnFamilyHandle *tx_cf,
          rocksdb::ColumnFamilyHandle *contract_cf)
    : db_(db), tx_cf_handle_(tx_cf), contract_cf_handle_(contract_cf)
  {}

  void stats_worker(std::stop_token &stoken) const;

  IndexerBlockApplyResult
  apply_validated_committed_block(const IndexerOutboxOrigin &origin,
                                  const IndexerCommittedBlock &block);

  IndexerStatus<void> initialize_progress_from_db();

  // True iff `h` is at or below the contiguous watermark or buffered
  // in pending_heights_.  The guard evidences ingest_mutex_ is held.
  bool height_seen(int64_t h, std::unique_lock<std::mutex> const &g) const;

  // Record an ingested height: advance the contiguous watermark
  // (draining newly-contiguous pending heights, persisting the meta
  // key) or buffer an out-of-order height.  The guard evidences
  // ingest_mutex_ is held.
  void record_height(int64_t h, std::unique_lock<std::mutex> const &g);

  std::unique_ptr<rocksdb::DB, RocksDBDeleter> db_;
  rocksdb::ColumnFamilyHandle *tx_cf_handle_;
  rocksdb::ColumnFamilyHandle *contract_cf_handle_;
  std::jthread stats_worker_thread_;

  // Sentinel for highest_indexed_block_: nothing indexed yet.
  static constexpr int64_t kNoBlocksIndexed = -1;

  // Serializes ingest tracking (pending_heights_ + watermark
  // advance/persist).  get_events never takes it — queries read only
  // the atomic below.  Default-constructed fresh on move (moves only
  // happen in the factory functions, before concurrent use).
  mutable std::mutex ingest_mutex_;

  // Heights ingested out of order, strictly above the watermark + 1.
  // Almost always empty (commit-time fanout delivers heights in
  // order); intentionally NOT persisted — a gap spanning a restart
  // stalls serving until upstream resends.  Guarded by ingest_mutex_.
  std::set<int64_t> pending_heights_;

  // Present only after durable epoch initialization. Guarded by ingest_mutex_.
  std::optional<IndexerInitializedProgress> durable_progress_;
  // Set when a recognized legacy watermark is loaded or legacy ingestion first
  // runs. This is distinct from a fresh empty database: durable initialization
  // must fail with an explicit migration-required outcome while legacy storage
  // remains restart-compatible.
  bool legacy_mode_ = false;
  std::function<void()> legacy_ingest_test_hook_;
  std::function<void()> initialize_epoch_test_hook_;
  std::function<void()> apply_validation_test_hook_;

  // Highest CONTIGUOUSLY ingested block height (see latest_block()),
  // kNoBlocksIndexed until the first ingest, seeded from the on-disk
  // meta key on open.  Atomic so get_events can read it without
  // synchronizing with the ingestion path.  Stored as int64 (the
  // underlying representation of BlockHeight) for atomic access.
  std::atomic<int64_t> highest_indexed_block_{kNoBlocksIndexed};
};

} // namespace hogd
