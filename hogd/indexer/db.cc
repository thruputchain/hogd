#include "hogd/indexer/db.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
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
#include <tuple>
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
#include "hogd/event/topic_layout.h"
#include "hogd/indexer/event_cursor.h"
#include "hogd/indexer/key_format.h"
#include "hogd/indexer/status.h"
#include "indexer.hh"
#include "pagination.hh"
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

  static_assert(std::endian::native == std::endian::little);

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

  IndexDB idx(db_ptr, tx_cf, contract_cf);
  idx.initialize_highest_block_from_db();
  return idx;
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
    stats_worker_thread_(std::move(other.stats_worker_thread_)),
    pending_heights_(std::move(other.pending_heights_)),
    highest_indexed_block_(
        other.highest_indexed_block_.load(std::memory_order_acquire))
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
    pending_heights_ = std::move(other.pending_heights_);
    highest_indexed_block_.store(other.highest_indexed_block_.load(
                                     std::memory_order_acquire),
                                 std::memory_order_release);
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

  {
    // Re-ingesting an already-seen height would overwrite served,
    // immutable history; skip it entirely.  Benign for byte-identical
    // retries; anything else is an upstream bug.
    std::lock_guard g(ingest_mutex_);
    if (height_seen_locked(static_cast<int64_t>(block_height))) {
      warn("consume_events: ignoring duplicate ingest for block {}",
           static_cast<int64_t>(block_height));
      return {};
    }
  }

  auto start_ts = x23::fastclock::get_ns();

  // Parallelism configuration
  static constexpr size_t kNumTxIndexChunks = 8;
  static constexpr size_t kNumContractIndexChunks = 8;

  // Get database path for temporary SST files.  We place them inside
  // the DB directory itself so they live and die with the DB, not in
  // the parent directory where concurrent indexer instances would
  // collide on filenames like `tx_index_{height}_{chunk}.sst`.
  std::string db_path = db_->GetName();
  std::filesystem::path db_dir(db_path);

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

  // Record the ingested height and advance the contiguous watermark.
  // Sequenced strictly AFTER the SST ingest above so the watermark
  // never claims a block whose rows aren't yet visible; the meta key
  // may lag the data (one-block window if we crash here) but can
  // never lead it.
  {
    std::lock_guard g(ingest_mutex_);
    record_height_locked(static_cast<int64_t>(block_height));
  }

  auto end_ts = x23::fastclock::get_ns();
  info("consume_events time: {} ns ({:.3f} s)", end_ts - start_ts,
       (end_ts - start_ts) / 1'000'000'000.0);

  return {};
}

bool
IndexDB::height_seen_locked(int64_t h) const
{
  const int64_t latest =
      highest_indexed_block_.load(std::memory_order_relaxed);
  return (latest != kNoBlocksIndexed && h <= latest) ||
         pending_heights_.contains(h);
}

void
IndexDB::record_height_locked(int64_t h)
{
  int64_t latest = highest_indexed_block_.load(std::memory_order_relaxed);
  if (latest != kNoBlocksIndexed && h != latest + 1) {
    // Out-of-order arrival above a gap: ingested, but not servable
    // until the gap fills.  Expected to be rare — commit-time fanout
    // delivers heights in order.
    warn("consume_events: out-of-order block {} buffered (watermark {})", h,
         latest);
    pending_heights_.insert(h);
    return;
  }
  // The first-ever ingested height becomes the base of the
  // contiguous range; afterwards drain any heights made contiguous
  // by this arrival.
  latest = h;
  auto it = pending_heights_.begin();
  while (it != pending_heights_.end() && *it == latest + 1) {
    latest = *it;
    it = pending_heights_.erase(it);
  }
  highest_indexed_block_.store(latest, std::memory_order_release);

  static_assert(std::endian::native == std::endian::little);
  const uint64_t encoded = std::byteswap(static_cast<uint64_t>(latest));
  const rocksdb::Status s =
      db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(),
               rocksdb::Slice(IndexerKeyFormat::kHighestIndexedBlockMetaKey),
               rocksdb::Slice(reinterpret_cast<const char *>(&encoded),
                              sizeof(encoded)));
  if (!s.ok()) {
    // Non-fatal: in-memory atomic still serves [latest_block]
    // correctly for the lifetime of this process.  Next successful
    // consume_events will re-attempt the Put.
    error("consume_events: failed to persist highest_indexed_block "
          "meta key: {}",
          s.ToString());
  }
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
  static_assert(std::endian::native == std::endian::little);
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

void
IndexDB::initialize_highest_block_from_db()
{
  // Read the meta key written by consume_events on every advance.
  // Default-CF Get is O(1) regardless of index size.  A missing key
  // means either an empty database or one that predates this meta
  // key; in both cases the default of 0 is correct (the next
  // consume_events will write the real value).
  if (!is_open()) {
    return;
  }
  std::string value;
  rocksdb::Status s =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
               rocksdb::Slice(IndexerKeyFormat::kHighestIndexedBlockMetaKey),
               &value);
  if (s.IsNotFound()) {
    return;
  }
  if (!s.ok()) {
    error("initialize_highest_block_from_db: meta-key Get failed: {}",
          s.ToString());
    return;
  }
  if (value.size() != sizeof(uint64_t)) {
    error("initialize_highest_block_from_db: meta-key value has wrong size "
          "({} bytes, expected {})",
          value.size(), sizeof(uint64_t));
    return;
  }
  static_assert(std::endian::native == std::endian::little);
  uint64_t encoded = 0;
  std::memcpy(&encoded, value.data(), sizeof(encoded));
  const auto h = static_cast<int64_t>(std::byteswap(encoded));
  highest_indexed_block_.store(h, std::memory_order_release);
}

BlockHeight
IndexDB::latest_block() const
{
  const int64_t w = highest_indexed_block_.load(std::memory_order_acquire);
  return static_cast<BlockHeight>(w == kNoBlocksIndexed ? 0 : w);
}

namespace {

// (block, tx, seq) — the lexicographic key of a single event in
// the contract_index, modulo the contract prefix.  Default
// `operator<=>` is the merge-order comparator used by the
// per-contract walk in get_events.
struct Position {
  BlockHeight block;
  SHA256Hash tx;
  std::uint32_t seq;
  auto operator<=>(Position const &) const = default;
};

// One RocksDB iterator plus the set of filter indices that include
// this contract.  Each iterator walks the contract_index slice for
// a single contract address in the requested direction.
class ContractIter {
public:
  ContractIter(rocksdb::DB *db, rocksdb::ColumnFamilyHandle *cf,
               ContractAddress addr, IterationOrder order,
               BlockHeight max_block)
    : contract_(addr), order_(order), max_block_(max_block),
      iter_(db->NewIterator(rocksdb::ReadOptions(), cf))
  {}

  ContractAddress const &contract() const { return contract_; }
  bool has_current() const { return has_current_; }
  Position const &current() const { return current_; }

  void add_filter(std::size_t fidx) { filter_indices_.push_back(fidx); }
  std::vector<std::size_t> const &filter_indices() const
  {
    return filter_indices_;
  }

  // Initial seek, treating `anchor` as an inclusive starting
  // boundary in the requested direction.
  void seek(BlockHeight anchor, IterationOrder order)
  {
    std::string prefix;
    IndexerKeyFormat::make_contract_key_prefix(prefix, contract_, anchor);
    if (order == IterationOrder::IterationOrderAscending) {
      iter_->Seek(prefix);
    }
    else {
      // Descending: we want entries at block <= anchor.  `prefix` lands
      // at the first key with block == anchor; append all-FF for the
      // (tx_hash, seq) suffix to push past those entries before
      // SeekForPrev so we land on (or below) the LAST event at
      // `anchor`.
      const std::size_t suffix_size = IndexerKeyFormat::kContractKeySize -
                                      IndexerKeyFormat::kContractKeyPrefixSize;
      std::string upper_prefix = prefix;
      upper_prefix.append(suffix_size, static_cast<char>(0xFF));
      iter_->SeekForPrev(upper_prefix);
    }
    refresh_current();
  }

  // Step one in the given direction.
  void advance(IterationOrder order)
  {
    if (!iter_->Valid()) {
      has_current_ = false;
      return;
    }
    if (order == IterationOrder::IterationOrderAscending) {
      iter_->Next();
    }
    else {
      iter_->Prev();
    }
    refresh_current();
  }

  // Advance past the cursor's last-emitted position (exclusive
  // resume).  After seek(anchor) with a cursor-encoded anchor,
  // skip the cursor entry itself if we landed on it.
  void skip_past(IterationOrder order, Position const &cursor)
  {
    while (has_current_) {
      const bool at_or_past_cursor =
          (order == IterationOrder::IterationOrderAscending)
              ? current_ >= cursor
              : current_ <= cursor;
      if (!at_or_past_cursor) {
        // Iterator is still behind the cursor — shouldn't happen
        // after a correct seek, but advance defensively.
        // advance(order) calls Next() ascending and Prev()
        // descending, so each iteration converges toward (and
        // eventually past) the cursor; the loop terminates once
        // the comparison flips or the iterator runs out.
        advance(order);
        continue;
      }
      if (current_ == cursor) {
        advance(order);
      }
      // Either we're already past the cursor, or we just advanced
      // past an exact-match entry; either way, current_ is now
      // strictly past the cursor (or has_current_ is false).
      return;
    }
  }

private:
  // Decode the iterator's current key into `current_`.  Stops
  // (has_current_=false) on iter end, on a key that has left the
  // contract's prefix range, or on a malformed key.
  void refresh_current()
  {
    if (!iter_->Valid()) {
      has_current_ = false;
      return;
    }
    rocksdb::Slice key = iter_->key();
    const std::string_view key_sv(key.data(), key.size());
    if (key_sv.size() < IndexerKeyFormat::kContractKeyPrefixSize ||
        !key_sv.starts_with(
            std::string_view(reinterpret_cast<const char *>(contract_.data()),
                             std::tuple_size_v<ContractAddress>))) {
      has_current_ = false;
      return;
    }
    ContractAddress addr_out;
    if (!IndexerKeyFormat::parse_contract_key(key_sv, &addr_out,
                                              &current_.block, &current_.tx,
                                              &current_.seq)) {
      has_current_ = false;
      return;
    }
    if (order_ == IterationOrder::IterationOrderAscending &&
        current_.block > max_block_) {
      // Rows above the contiguous watermark are not servable: they
      // (or a block below them) may still be ingesting, so they are
      // not yet immutable.  Ascending keys only grow, so this
      // iterator is exhausted for the current query.  Descending
      // never gets here — its seek anchor is clamped to the
      // watermark before iteration starts.
      has_current_ = false;
      return;
    }
    has_current_ = true;
  }

  ContractAddress contract_;
  IterationOrder order_;
  BlockHeight max_block_;
  std::unique_ptr<rocksdb::Iterator> iter_;
  std::vector<std::size_t> filter_indices_;
  Position current_{};
  bool has_current_ = false;
};

} // namespace

IndexerStatus<EventPage>
IndexDB::get_events(EventQuery const &query) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  EventPage page;
  // The serving bound for this entire query: rows above this height
  // are ingested-but-mutable (a lower gap may still be filling) and
  // must not be observed by the page or its continuation cursor.
  const BlockHeight max_serve_block = latest_block();
  page.latest_block = max_serve_block;
  page.oldest_block = oldest_block();

  // Validate: filters non-empty, each filter's contract_addresses
  // non-empty.  Empty contract_addresses is invalid because the
  // indexer has no topic-indexed column family (full-index scan).
  if (query.filters.empty()) {
    return page;
  }
  for (EventFilter const &f : query.filters) {
    if (f.contract_addresses.empty()) {
      return page;
    }
  }

  // Clamp limit.
  const uint32_t limit = IndexDB::clamp_events_limit(query.limit);

  // Decode cursor if present; else use the query's anchor/order.
  IterationOrder order;
  BlockHeight anchor_block;
  bool have_cursor = false;
  BlockHeight cursor_block{0};
  SHA256Hash cursor_tx{};
  uint32_t cursor_seq = 0;

  if (!query.cursor.empty()) {
    auto decoded = cursor::decode(
        std::string_view(reinterpret_cast<const char *>(query.cursor.data()),
                         query.cursor.size()));
    if (!decoded) {
      // Unknown version / malformed cursor.  Return empty page with
      // empty cursor so the client restarts pagination; better than
      // silently continuing with a misinterpreted position.
      return page;
    }
    if (!is_valid_iteration_order(decoded->order)) {
      return page;
    }
    order = decoded->order;
    cursor_block = decoded->block_height;
    cursor_tx = decoded->tx_hash;
    cursor_seq = decoded->sequence_number;
    have_cursor = true;
    anchor_block = cursor_block;
  }
  else {
    order = query.order;
    if (!is_valid_iteration_order(order)) {
      return page;
    }
    switch (query.anchor.kind()) {
    case BlockAnchorKind::AnchorAtBeginning:
      if (order == IterationOrder::IterationOrderDescending) {
        // Nonsense combination — nothing to iterate toward below
        // the oldest retained block.
        return page;
      }
      anchor_block = oldest_block();
      break;
    case BlockAnchorKind::AnchorAtEnd:
      if (order == IterationOrder::IterationOrderAscending) {
        // Nonsense combination — nothing to iterate toward above
        // the highest indexed block.
        return page;
      }
      anchor_block = max_serve_block;
      break;
    case BlockAnchorKind::AnchorAtHeight:
      anchor_block = query.anchor.height();
      if (anchor_block < oldest_block()) {
        if (order == IterationOrder::IterationOrderDescending) {
          // Everything at or below the anchor is outside the
          // retained range — empty page, per protocol/indexer.x's
          // oldest_block contract.  Validating here also keeps the
          // signed height from wrapping to all-0xFF in the seek key.
          return page;
        }
        anchor_block = oldest_block();
      }
      break;
    }
  }

  // Descending scans seek from the anchor downward; clamp the start
  // to the watermark so pending rows are never visited.  Ascending
  // scans are bounded inside ContractIter instead.
  if (order == IterationOrder::IterationOrderDescending &&
      anchor_block > max_serve_block) {
    anchor_block = max_serve_block;
  }

  // Dedupe contracts across all filters.  For each unique contract,
  // remember which filter indices included it — we'll run topic
  // post-filter against each when a candidate surfaces.
  std::vector<ContractIter> iters;
  for (std::size_t fidx = 0; fidx < query.filters.size(); ++fidx) {
    for (ContractAddress const &addr : query.filters[fidx].contract_addresses) {
      auto it = std::ranges::find(iters, addr, &ContractIter::contract);
      if (it != iters.end()) {
        it->add_filter(fidx);
      }
      else {
        iters.emplace_back(db_.get(), contract_cf_handle_, addr, order,
                           max_serve_block);
        iters.back().add_filter(fidx);
      }
    }
  }

  // Seek each iterator to the anchor.
  Position cursor_pos{.block = cursor_block,
                      .tx = cursor_tx,
                      .seq = cursor_seq};
  for (ContractIter &ci : iters) {
    ci.seek(anchor_block, order);
    if (have_cursor && ci.has_current()) {
      ci.skip_past(order, cursor_pos);
    }
  }

  // Merge-walk.  At each step, pick the iterator whose current
  // position is smallest (ascending) or largest (descending) and
  // consume it.  Stop at `limit` matches or exhaustion.
  Position last_emitted{};
  bool any_emitted = false;

  while (page.events.size() < limit) {
    // Find the winner — the valid iterator whose current position
    // leads in the requested direction.
    auto leads = [order](Position const &lhs, Position const &rhs) {
      return order == IterationOrder::IterationOrderAscending ? lhs < rhs
                                                              : lhs > rhs;
    };
    ContractIter *winner = nullptr;
    for (ContractIter &ci : iters) {
      if (!ci.has_current()) {
        continue;
      }
      if (winner == nullptr || leads(ci.current(), winner->current())) {
        winner = &ci;
      }
    }
    if (winner == nullptr) {
      break; // all exhausted
    }

    Position const win_pos = winner->current();

    // Hydrate.
    auto tx_events = get_tx_events(win_pos.tx);
    if (!tx_events) {
      error("get_events: tx_index miss for tx {} at block {}",
            HexArray(win_pos.tx).str(), static_cast<int64_t>(win_pos.block));
      winner->advance(order);
      continue;
    }
    const TxEventLog &event_log = tx_events->second;
    if (win_pos.seq >= event_log.events.size()) {
      error("get_events: seq {} out of range ({}) for tx {}", win_pos.seq,
            event_log.events.size(), HexArray(win_pos.tx).str());
      winner->advance(order);
      continue;
    }
    TxEmittedEvent const &emitted_event = event_log.events[win_pos.seq];

    // Topic filter: match if ANY filter that includes this contract
    // accepts the event.  One entry in the contract_index = one event
    // = one emission; since dedupe-by-contract is done above, we
    // can't double-emit.
    bool matched = false;
    for (std::size_t fidx : winner->filter_indices()) {
      if (event_topics_match(query.filters[fidx].topics, emitted_event)) {
        matched = true;
        break;
      }
    }

    if (matched) {
      IdentifiedEvent ev;
      ev.proposed_tx_hash = win_pos.tx;
      ev.block_height = win_pos.block;
      ev.sequence_number = win_pos.seq;
      ev.emitted_event = emitted_event;
      page.events.push_back(std::move(ev));
      last_emitted = win_pos;
      any_emitted = true;
    }

    winner->advance(order);
  }

  // Cursor for continuation: non-empty iff we emitted something AND
  // any iterator is still positioned at more data.  If all iterators
  // are exhausted, we return an empty cursor to signal end-of-stream.
  //
  // We signal empty cursor only when truly exhausted, to distinguish
  // "more-results-to-scan" (selective filter yielded no matches this
  // window) from "end of data".
  const bool any_more = std::ranges::any_of(iters, [](ContractIter const &ci) {
    return ci.has_current();
  });

  if (any_more) {
    if (any_emitted) {
      EventCursorPayload payload;
      payload.order = order;
      payload.block_height = last_emitted.block;
      payload.tx_hash = last_emitted.tx;
      payload.sequence_number = last_emitted.seq;
      std::string encoded = cursor::encode(payload);
      page.cursor.append_range(std::span<const uint8_t>(
          reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size()));
    }
    else {
      // Scanned without matches but more to check.  Synthesize a
      // cursor from the "frontier" — the position of the winner at
      // its last advance.  We don't have that here directly, but
      // re-seeking to (any iterator's current) would over-skip.  For
      // simplicity, use the input cursor (if any) or the anchor.
      // This means empty-match pages in cursor-less mode terminate
      // prematurely; clients should pass cursors forward even on
      // empty pages.  Cursor-based re-entry works correctly.
      if (have_cursor) {
        page.cursor = query.cursor;
      }
    }
  }

  return page;
}

} // namespace hogd
