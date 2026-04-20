#include "hogd/indexer/db.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <rocksdb/version.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <rocksdb/compression_type.h>
// NOLINTNEXTLINE(misc-header-include-cycle)
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

#include "event.hh"
#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/config.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/indexer/key_format.h"
#include "hogd/indexer/status.h"
#include "types.hh"
#include "x23/fastclock.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// Helper function to create a tx_index SST file chunk
IndexerStatus<bool>
create_tx_index_sst_chunk(
    rocksdb::DB * /*db*/, rocksdb::ColumnFamilyHandle *tx_cf_handle,
    BlockHeight block_height, std::span<const TxEventLog> events_chunk,
    const std::string &sst_path, const rocksdb::Options &sst_options,
    const rocksdb::EnvOptions &env_options, size_t chunk_idx)
{
  // Skip empty chunks - don't create SST file
  if (events_chunk.empty()) {
    return false;
  }

  // Collect entries for this chunk
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(events_chunk.size());
  std::string key_buffer; // Reusable buffer for key construction

  for (const auto &event_log : events_chunk) {
    // Serialize the TxEventLog for storage
    auto serialized = x23::serialize(event_log);
    std::string value(reinterpret_cast<const char *>(&block_height),
                      sizeof(block_height));
    value.append(std::string(reinterpret_cast<const char *>(serialized.data()),
                             serialized.size()));

    // Index by transaction (in tx_index column family)
    IndexerKeyFormat::make_tx_key(key_buffer, event_log.proposed_tx_hash);
    entries.emplace_back(key_buffer, value);
  }

  // Sort entries for this chunk
  std::ranges::sort(entries, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });

  // Write sorted entries to SST file
  rocksdb::SstFileWriter sst_writer(env_options, sst_options, tx_cf_handle);
  rocksdb::Status status = sst_writer.Open(sst_path);
  if (!status.ok()) {
    error("Failed to open tx_index SST file {}: {}", chunk_idx,
          status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  for (const auto &[key, value] : entries) {
    status = sst_writer.Put(key, value);
    if (!status.ok()) {
      error("Failed to add key to tx_index SST file {}: {}", chunk_idx,
            status.ToString());
      return IndexerError(IndexerErrorCode::kInternal);
    }
  }

  status = sst_writer.Finish();
  if (!status.ok()) {
    error("Failed to finish tx_index SST file {}: {}", chunk_idx,
          status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return true;
}

// Helper function to create a contract_index SST file chunk
IndexerStatus<bool>
create_contract_index_sst_chunk(
    rocksdb::DB * /*db*/, rocksdb::ColumnFamilyHandle *contract_cf_handle,
    BlockHeight block_height, std::span<const TxEventLog> events_chunk,
    const std::string &sst_path, const rocksdb::Options &sst_options,
    const rocksdb::EnvOptions &env_options, size_t chunk_idx)
{
  // Count total events in this chunk to reserve space
  size_t total_events = 0;
  for (const auto &event_log : events_chunk) {
    total_events += event_log.events.size();
  }

  // Skip empty chunks - don't create SST file
  if (total_events == 0) {
    return false;
  }

  // Collect entries for this chunk
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(total_events);
  std::string key_buffer; // Reusable buffer for key construction
  const std::string empty_string;

  for (const auto &event_log : events_chunk) {
    uint32_t sequence_number = 0;
    for (const auto &emitted_event : event_log.events) {
      IndexerKeyFormat::make_contract_key(key_buffer,
                                          emitted_event.contract_address,
                                          block_height,
                                          event_log.proposed_tx_hash,
                                          sequence_number);
      entries.emplace_back(key_buffer, empty_string);
      sequence_number++;
    }
  }

  // Sort entries for this chunk
  std::ranges::sort(entries, [](const auto &a, const auto &b) {
    return a.first < b.first;
  });

  // Write sorted entries to SST file
  rocksdb::SstFileWriter sst_writer(env_options, sst_options,
                                    contract_cf_handle);
  rocksdb::Status status = sst_writer.Open(sst_path);
  if (!status.ok()) {
    error("Failed to open contract_index SST file {}: {}", chunk_idx,
          status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  for (const auto &[key, value] : entries) {
    status = sst_writer.Put(key, value);
    if (!status.ok()) {
      error("Failed to add key to contract_index SST file {}: {}", chunk_idx,
            status.ToString());
      return IndexerError(IndexerErrorCode::kInternal);
    }
  }

  status = sst_writer.Finish();
  if (!status.ok()) {
    error("Failed to finish contract_index SST file {}: {}", chunk_idx,
          status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return true;
}

} // namespace

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

  // Check if database already exists by trying to list column families
  rocksdb::Options check_options;
  check_options.create_if_missing = false;
  std::vector<std::string> existing_column_families;
  rocksdb::Status list_status =
      rocksdb::DB::ListColumnFamilies(check_options, path.string(),
                                      &existing_column_families);

  if (list_status.ok()) {
    // Database already exists
    error("Database already exists at: {}", path.string());
    return IndexerError(IndexerErrorCode::kDBAlreadyExists);
  }

  // Database doesn't exist, create it
  // Create directory if it doesn't exist
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    error("Failed to create directory: {} ", ec.message());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  rocksdb::Options options;
  options.create_if_missing = true;
  options.compression = compression_algorithm;

  // Performance optimizations for bulk loading
  // Increase write buffer size for better SST file writing performance
  options.write_buffer_size = 256_MiB; // 256 MB (default is 64 MB)
  // Increase target file size for larger SST files (better for bulk loading)
  options.target_file_size_base = 256_MiB; // 256 MB (default is 64 MB)
  // Allow more write buffers in memory
  options.max_write_buffer_number = 6; // default is 2

  rocksdb::DB *db_ptr = nullptr;

  info("Making new indexer database at '{}'\nRocksDB build config: {}",
       path.string(), get_build_config());

  // First, open the database with just the default column family
  // This creates the database
  rocksdb::Status status = rocksdb::DB::Open(options, path.string(), &db_ptr);

  if (!status.ok()) {
    error("Failed to create database: {} ", status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  // Now create the additional column families
  rocksdb::ColumnFamilyOptions cf_options(options);
  rocksdb::ColumnFamilyHandle *tx_cf = nullptr;
  rocksdb::ColumnFamilyHandle *contract_cf = nullptr;

  status = db_ptr->CreateColumnFamily(cf_options,
                                      IndexerKeyFormat::kTxIndexColumnFamily,
                                      &tx_cf);
  if (!status.ok()) {
    error("Failed to create tx_index column family: {} ", status.ToString());
    delete db_ptr;
    return IndexerError(IndexerErrorCode::kInternal);
  }

  status =
      db_ptr->CreateColumnFamily(cf_options,
                                 IndexerKeyFormat::kContractIndexColumnFamily,
                                 &contract_cf);
  if (!status.ok()) {
    error("Failed to create contract_index column family: {} ",
          status.ToString());
    db_ptr->DestroyColumnFamilyHandle(tx_cf);
    delete db_ptr;
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return IndexDB(db_ptr, tx_cf, contract_cf);
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

  // List existing column families
  std::vector<std::string> column_family_names;
  rocksdb::Status status =
      rocksdb::DB::ListColumnFamilies(options, path.string(),
                                      &column_family_names);

  if (!status.ok()) {
    error("Failed to list column families: {} ", status.ToString());
    if (status.IsInvalidArgument() || status.IsNotFound() ||
        status.IsIOError()) {
      return IndexerError(IndexerErrorCode::kDBNotFound);
    }
    return IndexerError(IndexerErrorCode::kInternal);
  }

  // Open all column families
  std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
  column_families.reserve(column_family_names.size());
  for (const auto &name : column_family_names) {
    column_families.emplace_back(name, rocksdb::ColumnFamilyOptions(options));
  }

  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  rocksdb::DB *db_ptr = nullptr;

  info("Opening indexer database at '{}'\nRocksDB build config: {}",
       path.string(), get_build_config());

  status = rocksdb::DB::Open(options, path.string(), column_families, &handles,
                             &db_ptr);

  if (!status.ok()) {
    error("Failed to open database: {} ", status.ToString());
    if (status.IsInvalidArgument() || status.IsNotFound()) {
      return IndexerError(IndexerErrorCode::kDBNotFound);
    }
    return IndexerError(IndexerErrorCode::kInternal);
  }

  // Find the column family handles
  rocksdb::ColumnFamilyHandle *tx_cf = nullptr;
  rocksdb::ColumnFamilyHandle *contract_cf = nullptr;

  for (size_t i = 0; i < column_family_names.size(); ++i) {
    if (column_family_names[i] == IndexerKeyFormat::kTxIndexColumnFamily) {
      tx_cf = handles[i];
    }
    else if (column_family_names[i] ==
             IndexerKeyFormat::kContractIndexColumnFamily) {
      contract_cf = handles[i];
    }
    else {
      // unneeded column family handle; delete it
      db_ptr->DestroyColumnFamilyHandle(handles[i]);
    }
  }

  if (tx_cf == nullptr || contract_cf == nullptr) {
    error("Missing required column families");
    delete db_ptr;
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return IndexDB(db_ptr, tx_cf, contract_cf);
}

IndexDB::~IndexDB()
{
  // Stop stats worker thread
  if (stats_worker_thread_.joinable()) {
    stats_worker_thread_.request_stop();
    stats_worker_thread_.join();
  }

  if (db_ != nullptr) {
    // Destroy column family handles before deleting the database
    if (tx_cf_handle_ != nullptr) {
      db_->DestroyColumnFamilyHandle(tx_cf_handle_);
      tx_cf_handle_ = nullptr;
    }
    if (contract_cf_handle_ != nullptr) {
      db_->DestroyColumnFamilyHandle(contract_cf_handle_);
      contract_cf_handle_ = nullptr;
    }
  }
  // db_ will be deleted by unique_ptr's deleter
}

IndexDB::IndexDB(IndexDB &&other) noexcept
  : db_(std::move(other.db_)), tx_cf_handle_(other.tx_cf_handle_),
    contract_cf_handle_(other.contract_cf_handle_),
    stats_worker_thread_(std::move(other.stats_worker_thread_))
{
  other.tx_cf_handle_ = nullptr;
  other.contract_cf_handle_ = nullptr;
}

IndexDB &
IndexDB::operator=(IndexDB &&other) noexcept
{
  if (this != &other) {
    // Stop existing stats worker thread
    if (stats_worker_thread_.joinable()) {
      stats_worker_thread_.request_stop();
      stats_worker_thread_.join();
    }

    // Clean up existing handles
    if (db_ != nullptr) {
      if (tx_cf_handle_ != nullptr) {
        db_->DestroyColumnFamilyHandle(tx_cf_handle_);
      }
      if (contract_cf_handle_ != nullptr) {
        db_->DestroyColumnFamilyHandle(contract_cf_handle_);
      }
    }
    // Move from other
    db_ = std::move(other.db_);
    tx_cf_handle_ = other.tx_cf_handle_;
    contract_cf_handle_ = other.contract_cf_handle_;
    stats_worker_thread_ = std::move(other.stats_worker_thread_);
    other.tx_cf_handle_ = nullptr;
    other.contract_cf_handle_ = nullptr;
  }
  return *this;
}

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

IndexerStatus<void>
IndexDB::consume_events(BlockHeight block_height,
                        std::span<const TxEventLog> events)
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  auto start_ts = x23::fastclock::get_ns();

  // Parallelism configuration
  static constexpr size_t kNumTxIndexChunks = 8;
  static constexpr size_t kNumContractIndexChunks = 8;

  // Get database path for temporary SST files
  // We'll create them in the same directory as the database
  std::string db_path = db_->GetName();
  std::filesystem::path db_path_fs(db_path);
  std::filesystem::path db_dir = db_path_fs.parent_path();

  // Create temporary SST file paths for tx_index
  std::vector<std::string> tx_sst_paths;
  tx_sst_paths.reserve(kNumTxIndexChunks);
  for (size_t i = 0; i < kNumTxIndexChunks; i++) {
    tx_sst_paths.push_back(
        (db_dir / std::format("tx_index_{}_{}.sst", block_height, i)).string());
  }

  // Create temporary SST file paths for contract_index
  std::vector<std::string> contract_sst_paths;
  contract_sst_paths.reserve(kNumContractIndexChunks);
  for (size_t i = 0; i < kNumContractIndexChunks; i++) {
    contract_sst_paths.push_back(
        (db_dir / std::format("contract_index_{}_{}.sst", block_height, i))
            .string());
  }

  // Get options from column family handles to ensure consistency
  rocksdb::Options tx_sst_options = db_->GetOptions(tx_cf_handle_);
  rocksdb::Options contract_sst_options = db_->GetOptions(contract_cf_handle_);

  // Enable paranoid file checks in debug builds to verify key sortedness
  // This will reopen and read back the SST file to verify keys are in order
#ifndef NDEBUG
  tx_sst_options.paranoid_file_checks = true;
  contract_sst_options.paranoid_file_checks = true;
#endif

  // Configure EnvOptions for better SST file writing performance
  rocksdb::EnvOptions env_options;
  // Use direct I/O for writes (bypasses OS page cache, better for large
  // sequential writes)
  env_options.use_direct_writes = true; // Set to true if you have fast storage
  // Sync data incrementally to disk (0 = disabled, larger = more frequent
  // syncs) For bulk loading, we can disable this since we're not concerned with
  // durability
  env_options.bytes_per_sync = 0; // Disable incremental sync for bulk loading
  // Allow fallocate for pre-allocating file space (can improve write
  // performance)
  env_options.allow_fallocate = true;

  // Create worker pool and submit jobs for each column family
  auto &pool = WorkerPool::get_default_pool();

  // Create tx_index SST files in parallel (one per chunk)
  // Divide events into chunks first, then each thread collects, sorts, and
  // writes its chunk
  std::vector<std::future<IndexerStatus<bool>>> tx_futures;
  tx_futures.reserve(kNumTxIndexChunks);

  const auto tx_events_chunk_size =
      divide_up<size_t>(events.size(), kNumTxIndexChunks);

  for (size_t i = 0; i < kNumTxIndexChunks; i++) {
    const size_t start_idx = i * tx_events_chunk_size;
    if (start_idx >= events.size()) {
      break;
    }
    const size_t end_idx =
        std::min(start_idx + tx_events_chunk_size, events.size());
    const auto events_chunk = events.subspan(start_idx, end_idx - start_idx);
    const std::string &chunk_sst_path = tx_sst_paths[i];

    auto *db_ptr = db_.get();
    CHECK_PTR(db_ptr);

    auto future = pool.submit([db = db_ptr, tx_cf = tx_cf_handle_, block_height,
                               events_chunk, &chunk_sst_path, &tx_sst_options,
                               &env_options, i]() -> IndexerStatus<bool> {
      return create_tx_index_sst_chunk(db, tx_cf, block_height, events_chunk,
                                       chunk_sst_path, tx_sst_options,
                                       env_options, i);
    });

    tx_futures.push_back(std::move(future));
  }

  // Create contract_index SST files in parallel (one per chunk)
  // Divide events into chunks first, then each thread collects, sorts, and
  // writes its chunk
  std::vector<std::future<IndexerStatus<bool>>> contract_futures;
  contract_futures.reserve(kNumContractIndexChunks);

  const auto contract_events_chunk_size =
      divide_up<size_t>(events.size(), kNumContractIndexChunks);

  for (size_t i = 0; i < kNumContractIndexChunks; i++) {
    const size_t start_idx = i * contract_events_chunk_size;
    if (start_idx >= events.size()) {
      break;
    }
    const size_t end_idx =
        std::min(start_idx + contract_events_chunk_size, events.size());
    const auto events_chunk = events.subspan(start_idx, end_idx - start_idx);
    const std::string &chunk_sst_path = contract_sst_paths[i];

    auto *db_ptr = db_.get();
    CHECK_PTR(db_ptr);

    auto future = pool.submit([db = db_ptr, contract_cf = contract_cf_handle_,
                               block_height, events_chunk, &chunk_sst_path,
                               &contract_sst_options, &env_options,
                               i]() -> IndexerStatus<bool> {
      try {
        return create_contract_index_sst_chunk(db, contract_cf, block_height,
                                               events_chunk, chunk_sst_path,
                                               contract_sst_options,
                                               env_options, i);
      } catch (...) {
        return IndexerError(IndexerErrorCode::kInternal);
      }
    });

    contract_futures.push_back(std::move(future));
  }

  // wait for all SST files to be created
  IndexerStatus<void> sst_error = {};

  // Collect only filenames for SST files that were actually created
  std::vector<std::string> created_tx_sst_paths;
  std::vector<std::string> created_contract_sst_paths;

  // Wait for all tx_index SST files to be created
  for (size_t i = 0; i < tx_futures.size(); i++) {
    auto result = tx_futures[i].get();
    if (!result.has_value()) {
      sst_error = result.error();
      continue;
    }
    // Only add filename if the SST file was actually created (bool is true)
    if (result.value()) {
      created_tx_sst_paths.push_back(tx_sst_paths[i]);
    }
  }

  // Wait for all contract index chunks
  for (size_t i = 0; i < contract_futures.size(); i++) {
    auto result = contract_futures[i].get();
    if (!result.has_value()) {
      sst_error = result.error();
      continue;
    }
    // Only add filename if the SST file was actually created (bool is true)
    if (result.value()) {
      created_contract_sst_paths.push_back(contract_sst_paths[i]);
    }
  }

  if (!sst_error.has_value()) {
    // clean up intermediate files
    for (const auto &file : tx_sst_paths) {
      std::filesystem::remove(file);
    }
    for (const auto &file : contract_sst_paths) {
      std::filesystem::remove(file);
    }
    return sst_error;
  }

  // Ingest the SST files into RocksDB
  rocksdb::IngestExternalFileOptions ingest_options;
  ingest_options.move_files = true; // Move files instead of copying
  ingest_options.snapshot_consistency =
      false; // We don't need snapshot consistency for indexer
  ingest_options.allow_global_seqno =
      true; // Allow global seqno assignment (default, but explicit)

  // Ingest all tx_index SST files at once (only non-empty ones)
  if (!created_tx_sst_paths.empty()) {
    rocksdb::Status status =
        db_->IngestExternalFile(tx_cf_handle_, created_tx_sst_paths,
                                ingest_options);
    if (!status.ok()) {
      error("Failed to ingest tx_index SST files: {}", status.ToString());
      for (const auto &file : tx_sst_paths) {
        std::filesystem::remove(file);
      }
      for (const auto &file : contract_sst_paths) {
        std::filesystem::remove(file);
      }
      return IndexerError(IndexerErrorCode::kInternal);
    }

    // Verify files were moved (since move_files = true)
    for (const auto &file : created_tx_sst_paths) {
      if (std::filesystem::exists(file)) {
        panic(
            "tx_index SST file {} still exists after ingestion (expected to be "
            "moved)",
            file);
      }
    }
  }

  // Ingest all contract index SST files at once (only non-empty ones)
  if (!created_contract_sst_paths.empty()) {
    rocksdb::Status status =
        db_->IngestExternalFile(contract_cf_handle_, created_contract_sst_paths,
                                ingest_options);
    if (!status.ok()) {
      error("Failed to ingest contract_index SST files: {}", status.ToString());
      return IndexerError(IndexerErrorCode::kInternal);
    }
    // Verify files were moved (since move_files = true)
    for (const auto &file : created_contract_sst_paths) {
      if (std::filesystem::exists(file)) {
        panic(
            "contract_index SST file {} still exists after ingestion (expected "
            "to be moved)",
            file);
      }
    }
  }

  auto end_ts = x23::fastclock::get_ns();
  info("consume_events time: {} ns ({:.3f} s)", end_ts - start_ts,
       (end_ts - start_ts) / 1'000'000'000.0);

  return {};
}

void
IndexDB::launch_stats_worker()
{
  if (!stats_worker_thread_.joinable()) {
    stats_worker_thread_ =
        std::jthread([this](std::stop_token stoken) { stats_worker(stoken); });
  }
}

void
IndexDB::stats_worker(std::stop_token &stoken) const
{
  std::mutex cv_mutex;
  std::condition_variable_any cv;

  while (!stoken.stop_requested()) {
    std::unique_lock g(cv_mutex);
    cv.wait_for(g, stoken, std::chrono::seconds(1), [] { return false; });

    if (!stoken.stop_requested() && is_open()) {
      log_stats();
    }
  }
}

void
IndexDB::log_stats() const
{
  if (!is_open()) {
    return;
  }

  uint64_t value;
  if (db_->GetAggregatedIntProperty(rocksdb::DB::Properties::kTotalSstFilesSize,
                                    &value)) {
    info("RocksDB total SST files size: {} bytes ({:.2f} MB)", value,
         value / (1024.0 * 1024.0));
  }
  if (db_->GetAggregatedIntProperty(rocksdb::DB::Properties::kLiveSstFilesSize,
                                    &value)) {
    info("RocksDB live SST files size: {} bytes ({:.2f} MB)", value,
         value / (1024.0 * 1024.0));
  }
  if (db_->GetAggregatedIntProperty(rocksdb::DB::Properties::kSizeAllMemTables,
                                    &value)) {
    info("RocksDB total memtable size: {} bytes ({:.2f} MB)", value,
         value / (1024.0 * 1024.0));
  }
  if (db_->GetAggregatedIntProperty(
          rocksdb::DB::Properties::kCurSizeAllMemTables, &value)) {
    info("RocksDB unflushed memtable size: {} bytes ({:.2f} MB)", value,
         value / (1024.0 * 1024.0));
  }
  if (db_->GetAggregatedIntProperty(
          rocksdb::DB::Properties::kEstimateTableReadersMem, &value)) {
    info("RocksDB table readers memory: {} bytes ({:.2f} MB)", value,
         value / (1024.0 * 1024.0));
  }
  if (db_->GetAggregatedIntProperty(rocksdb::DB::Properties::kEstimateNumKeys,
                                    &value)) {
    info("RocksDB estimated number of keys: {}", value);
  }
}

IndexerStatus<std::pair<BlockHeight, TxEventLog>>
IndexDB::get_tx_events(SHA256Hash const &tx_hash) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  std::string key_buffer;
  IndexerKeyFormat::make_tx_key(key_buffer, tx_hash);
  std::string value;

  rocksdb::ReadOptions read_options;
  rocksdb::Status status =
      db_->Get(read_options, tx_cf_handle_, key_buffer, &value);

  if (status.IsNotFound()) {
    return IndexerError(IndexerErrorCode::kDBNotFound);
  }

  if (!status.ok()) {
    error("Failed to read from indexer: {}", status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  // TODO(jhumphri): Support different endianness.
  CHECK_GE(value.size(), sizeof(BlockHeight));
  BlockHeight block_height;
  // value.data() is not guaranteed to be 8-byte aligned
  // hence we use std::memcpy instead of
  // *reinterpret_cast<BlockHeight*>(value.data())
  std::memcpy(&block_height, value.data(), sizeof(BlockHeight));
  TxEventLog event_log;
  try {
    x23::deserialize(&event_log,
                     x23::BufView{reinterpret_cast<const uint8_t *>(
                                      value.data() + sizeof(BlockHeight)),
                                  value.size() - sizeof(BlockHeight)});
  } catch (const x23::XdrBadInput &e) {
    error("Failed to deserialize TxEventLog: {}", e.what());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return std::make_pair(block_height, event_log);
}

IndexerStatus<std::vector<IdentifiedEvent>>
IndexDB::get_contract_events(BlockHeight start_height, BlockHeight end_height,
                             ContractAddress const &contract_address) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  std::vector<IdentifiedEvent> result;
  std::string key_buffer; // Reusable buffer for key construction

  // Create prefix for the contract address at start_height
  IndexerKeyFormat::make_contract_key_prefix(key_buffer, contract_address,
                                             start_height);
  std::string start_prefix = key_buffer;

  // Create prefix for the contract address at end_height+1 (exclusive end)
  IndexerKeyFormat::make_contract_key_prefix(key_buffer, contract_address,
                                             end_height + 1);
  std::string end_prefix = key_buffer;

  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(read_options, contract_cf_handle_));

  // Seek to start and iterate until we exceed end_height
  for (it->Seek(start_prefix); it->Valid(); it->Next()) {
    rocksdb::Slice key_slice = it->key();

    // Check if we've exceeded the end prefix
    if (key_slice.compare(end_prefix) >= 0) {
      break;
    }

    // Verify the key starts with the contract address
    if (key_slice.size() < 32) {
      continue;
    }
    std::string_view key_addr(key_slice.data(), 32);
    std::string_view expected_addr(reinterpret_cast<const char *>(
                                       contract_address.data()),
                                   32);
    if (key_addr != expected_addr) {
      break; // We've moved past this contract's keys
    }

    // Parse the key to extract block_height, tx_hash, and sequence_number
    std::string_view key_str(key_slice.data(), key_slice.size());
    BlockHeight block_height;
    SHA256Hash tx_hash;
    uint32_t sequence_number;

    if (!IndexerKeyFormat::parse_contract_key(key_str, nullptr, &block_height,
                                              &tx_hash, &sequence_number)) {
      error("Failed to parse contract key");
      continue;
    }

    // Filter by height range
    if (block_height < start_height || block_height > end_height) {
      continue;
    }

    // Look up the transaction events from tx_index
    auto tx_events_result = get_tx_events(tx_hash);
    if (!tx_events_result.has_value()) {
      error("Failed to look up transaction events for tx_hash {}",
            HexArray(tx_hash).str());
      continue;
    }

    const TxEventLog &event_log = tx_events_result->second;

    // Extract the event at the sequence_number
    if (sequence_number >= event_log.events.size()) {
      error("Sequence number {} out of range for transaction ({} events)",
            sequence_number, event_log.events.size());
      continue;
    }

    // Create IdentifiedEvent
    IdentifiedEvent identified_event;
    identified_event.proposed_tx_hash = tx_hash;
    identified_event.block_height = block_height;
    identified_event.sequence_number = sequence_number;
    identified_event.emitted_event = event_log.events[sequence_number];

    result.push_back(identified_event);
  }

  return result;
}

} // namespace hogd
