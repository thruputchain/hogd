#pragma once

#include <filesystem>
#include <memory>
#include <rocksdb/compression_type.h>
#include <span>
#include <string>
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

  ~IndexDB();

  // Delete copy, allow move
  IndexDB(const IndexDB &) = delete;
  IndexDB &operator=(const IndexDB &) = delete;
  IndexDB(IndexDB &&) noexcept;
  IndexDB &operator=(IndexDB &&) noexcept;

  static std::string get_build_config();

  // Check if database is open
  bool is_open() const { return db_ != nullptr; }

  static bool
  is_compression_supported(rocksdb::CompressionType compression_algorithm);

  // Consume a batch of events directly
  // Indexes the provided events for the given block_height
  IndexerStatus<void> consume_events(BlockHeight block_height,
                                     std::span<const TxEventLog> events);

  // Lookup events for a specific transaction in a block
  // Returns the TxEventLog if found, or an error if not found
  IndexerStatus<TxEventLog> get_tx_events(BlockHeight block_height,
                                          SHA256Hash const &tx_hash) const;

  // Get all events for a specific contract address
  // Returns a vector of IdentifiedEvent entries
  IndexerStatus<std::vector<IdentifiedEvent>>
  get_contract_events(BlockHeight start_height, BlockHeight end_height,
                      ContractAddress const &contract_address) const;

  // Log RocksDB statistics (memory usage, database size, etc.)
  void log_stats() const;

private:
  IndexDB() = default;
  IndexDB(rocksdb::DB *db, rocksdb::ColumnFamilyHandle *tx_cf,
          rocksdb::ColumnFamilyHandle *contract_cf)
    : db_(db), tx_cf_handle_(tx_cf), contract_cf_handle_(contract_cf)
  {}

  std::unique_ptr<rocksdb::DB, RocksDBDeleter> db_;
  rocksdb::ColumnFamilyHandle *tx_cf_handle_;
  rocksdb::ColumnFamilyHandle *contract_cf_handle_;
};

} // namespace hogd
