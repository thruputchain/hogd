#pragma once

#include <filesystem>
#include <memory>
#include <rocksdb/compression_type.h>
#include <string>

#include "hogd/indexer/status.h"

// Forward declare RocksDB types to avoid including headers here
namespace rocksdb {
class DB;
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

private:
  IndexDB() = default;
  explicit IndexDB(rocksdb::DB *db) : db_(db) {}

  std::unique_ptr<rocksdb::DB, RocksDBDeleter> db_;
};

} // namespace hogd
