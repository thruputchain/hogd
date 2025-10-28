#include "hogd/indexer/db.h"

#include <filesystem>
#include <rocksdb/version.h>
#include <string>
#include <system_error>

#include <rocksdb/compression_type.h>
// NOLINTNEXTLINE(misc-header-include-cycle)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/status.h>

#include "hogd/base/log.h"
#include "hogd/config.h"
#include "hogd/indexer/status.h"

namespace hogd {

void
RocksDBDeleter::operator()(rocksdb::DB *db) const
{
  delete db;
}

IndexerStatus<IndexDB>
IndexDB::make_new(const std::filesystem::path &path,
                  rocksdb::CompressionType compression_algorithm)
{
  if (!is_compression_supported(compression_algorithm)) {
    return IndexerError(IndexerErrorCode::kUnsupportedCompressionAlgorithm);
  }
  // Create directory if it doesn't exist
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    error("Failed to create directory: {} ", ec.message());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  rocksdb::Options options;
  options.create_if_missing = true;
  options.error_if_exists = true; // Fail if database already exists

  options.compression = compression_algorithm;

  info("Making new indexer database at '{}'\nRocksDB build config: {}",
       path.string(), get_build_config());

  rocksdb::DB *db_ptr = nullptr;
  rocksdb::Status status = rocksdb::DB::Open(options, path.string(), &db_ptr);

  if (status.ok()) {
    return IndexDB(db_ptr);
  }
  error("Failed to open database: {} ", status.ToString());
  if (status.IsInvalidArgument()) {
    return IndexerError(IndexerErrorCode::kDBAlreadyExists);
  }

  return IndexerError(IndexerErrorCode::kInternal);
}

IndexerStatus<IndexDB>
IndexDB::open(const std::filesystem::path &path,
              rocksdb::CompressionType compression_algorithm)
{
  if (!is_compression_supported(compression_algorithm)) {
    return IndexerError(IndexerErrorCode::kUnsupportedCompressionAlgorithm);
  }

  rocksdb::Options options;
  options.create_if_missing = false; // Don't create if it doesn't exist

  options.compression = compression_algorithm;

  rocksdb::DB *db_ptr = nullptr;
  rocksdb::Status status = rocksdb::DB::Open(options, path.string(), &db_ptr);

  info("Opening indexer database at '{}'\nRocksDB build config: {}",
       path.string(), get_build_config());

  if (status.ok()) {
    return IndexDB(db_ptr);
  }

  error("Failed to open database: {} ", status.ToString());
  if (status.IsInvalidArgument()) {
    return IndexerError(IndexerErrorCode::kDBNotFound);
  }
  return IndexerError(IndexerErrorCode::kInternal);
}

IndexDB::IndexDB(IndexDB &&) noexcept = default;
IndexDB &IndexDB::operator=(IndexDB &&) noexcept = default;

std::string
IndexDB::get_build_config()
{
  // Detailed build config logs are written to the LOG file in the db directory.
  return rocksdb::GetRocksBuildInfoAsString("", true);
}

bool
IndexDB::is_compression_supported(
    rocksdb::CompressionType compression_algorithm)
{
  // NOLINTBEGIN(bugprone-branch-clone)
  switch (compression_algorithm) {
  case rocksdb::CompressionType::kNoCompression:
    return true;
  case rocksdb::CompressionType::kSnappyCompression:
    return ROCKSDB_WITH_SNAPPY;
  case rocksdb::CompressionType::kZlibCompression:
    return ROCKSDB_WITH_ZLIB;
  case rocksdb::CompressionType::kBZip2Compression:
    return ROCKSDB_WITH_BZ2;
  case rocksdb::CompressionType::kLZ4Compression:
    return ROCKSDB_WITH_LZ4;
  case rocksdb::CompressionType::kLZ4HCCompression:
    return ROCKSDB_WITH_LZ4;
  case rocksdb::CompressionType::kXpressCompression:
    warn("Not set up to check for Xpress compression support");
    return false;
  case rocksdb::CompressionType::kZSTD:
    return ROCKSDB_WITH_ZSTD;
  default:
    warn("Unknown compression algorithm: {}",
         static_cast<int>(compression_algorithm));
    return false;
  }
  // NOLINTEND(bugprone-branch-clone)
}

} // namespace hogd
