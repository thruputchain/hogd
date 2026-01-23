#pragma once

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
    else {
      // Nothing exists at [path], so create a new database there.
      return make_new(path, compression_algorithm);
    }
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

  // Get all events for a specific contract address
  // Returns a vector of IdentifiedEvent entries
  IndexerStatus<std::vector<IdentifiedEvent>>
  get_contract_events(BlockHeight start_height, BlockHeight end_height,
                      ContractAddress const &contract_address) const;

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

  std::unique_ptr<rocksdb::DB, RocksDBDeleter> db_;
  rocksdb::ColumnFamilyHandle *tx_cf_handle_;
  rocksdb::ColumnFamilyHandle *contract_cf_handle_;
  std::jthread stats_worker_thread_;
};

} // namespace hogd
