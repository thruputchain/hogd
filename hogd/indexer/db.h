#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <rocksdb/compression_type.h>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "event.hh"
#include "hogd/indexer/status.h"
#include "indexer.hh"
#include "types.hh"

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
  IndexDB(IndexDB &&other) noexcept;
  IndexDB &operator=(IndexDB &&other) noexcept;

  static std::string get_build_config();

  // Check if database is open
  bool is_open() const { return db_ != nullptr; }

  static bool
  is_compression_supported(rocksdb::CompressionType compression_algorithm);

  // Consume a batch of events directly
  // Indexes the provided events for the given block_height
  IndexerStatus<void> consume_events(BlockHeight block_height,
                                     std::span<const TxEventLog> events);

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

  // Highest block height the indexer has processed, or BlockHeight(0)
  // if nothing has been indexed yet.  Updated on every consume_events
  // call; lock-free atomic, safe to read concurrently with indexing.
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

  // Seek the tx_index to its last entry to initialize
  // highest_indexed_block_ on open.  Runs once at construction.
  void initialize_highest_block_from_db();

  std::unique_ptr<rocksdb::DB, RocksDBDeleter> db_;
  rocksdb::ColumnFamilyHandle *tx_cf_handle_;
  rocksdb::ColumnFamilyHandle *contract_cf_handle_;
  std::jthread stats_worker_thread_;

  // Highest block height seen by consume_events() since the DB was
  // opened, capped below by whatever was present on disk at open
  // time.  Atomic so get_events can read it without synchronizing
  // with the ingestion path.  Stored as int64 (the underlying
  // representation of BlockHeight) for atomic access.
  std::atomic<int64_t> highest_indexed_block_{0};
};

} // namespace hogd
