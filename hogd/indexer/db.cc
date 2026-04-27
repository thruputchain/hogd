#include "hogd/indexer/db.h"

#include <algorithm>
#include <atomic>
#include <bit>
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

  // Bump the latest-block high-water mark.  Monotonic — ingestion is
  // sequential at block boundaries, but a compare-exchange guards
  // against any future concurrency and keeps the invariant obvious.
  // When we win the CAS, persist the new value via the meta key so
  // [latest_block] survives restart accurately.  We sequence the Put
  // strictly AFTER the SST ingest above: the meta key may lag the data
  // (one-block window if we crash here) but can never lead it.
  {
    const auto new_h = static_cast<int64_t>(block_height);
    int64_t cur = highest_indexed_block_.load(std::memory_order_acquire);
    bool advanced = false;
    while (new_h > cur) {
      if (highest_indexed_block_.compare_exchange_weak(
              cur, new_h, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        advanced = true;
        break;
      }
    }
    if (advanced) {
      uint64_t encoded = std::byteswap(static_cast<uint64_t>(new_h));
      rocksdb::Status s =
          db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(),
                   rocksdb::Slice(
                       IndexerKeyFormat::kHighestIndexedBlockMetaKey),
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
  uint64_t encoded = 0;
  std::memcpy(&encoded, value.data(), sizeof(encoded));
  const auto h = static_cast<int64_t>(std::byteswap(encoded));
  highest_indexed_block_.store(h, std::memory_order_release);
}

BlockHeight
IndexDB::latest_block() const
{
  return static_cast<BlockHeight>(
      highest_indexed_block_.load(std::memory_order_acquire));
}

namespace {

// Decode a contract_index key into its (contract, block, tx, seq)
// components.  Returns false on malformed input.
bool
parse_contract_key_full(std::string_view key, ContractAddress *address,
                        BlockHeight *block_height, SHA256Hash *tx_hash,
                        uint32_t *seq_number)
{
  if (key.size() < IndexerKeyFormat::kContractKeySize) {
    return false;
  }
  const auto *p = reinterpret_cast<const uint8_t *>(key.data());
  if (address != nullptr) {
    std::memcpy(address->data(), p, 32);
  }
  int64_t h = 0;
  for (size_t i = 0; i < 8; ++i) {
    h = (h << 8) | p[32 + i];
  }
  *block_height = static_cast<BlockHeight>(h);
  std::memcpy(tx_hash->data(), p + 40, 32);
  uint32_t s = 0;
  for (size_t i = 0; i < 4; ++i) {
    s = (s << 8) | p[72 + i];
  }
  *seq_number = s;
  return true;
}

// One RocksDB iterator plus the set of filter indices that include
// this contract.  Each iterator walks the contract_index slice for
// a single contract address in the requested direction.
struct ContractIter {
  ContractAddress contract;
  std::unique_ptr<rocksdb::Iterator> iter;
  // filters that include this contract; used for topic post-filter.
  std::vector<size_t> filter_indices;
  // cached current position (decoded on last valid advance).
  BlockHeight current_block = static_cast<BlockHeight>(0);
  SHA256Hash current_tx_hash{};
  uint32_t current_seq = 0;
  bool has_current = false;
};

// True iff a < b under lexicographic ordering of (block, tx_hash,
// sequence_number).  Used to pick the merge winner in ascending
// iteration; for descending, pass the operands in reverse.
bool
position_less(BlockHeight a_block, SHA256Hash const &a_tx, uint32_t a_seq,
              BlockHeight b_block, SHA256Hash const &b_tx, uint32_t b_seq)
{
  if (a_block != b_block) {
    return static_cast<int64_t>(a_block) < static_cast<int64_t>(b_block);
  }
  const int cmp = std::memcmp(a_tx.data(), b_tx.data(), 32);
  if (cmp != 0) {
    return cmp < 0;
  }
  return a_seq < b_seq;
}

// Advance an iterator one step in the given direction, updating its
// has_current / current_* fields.  Stops (has_current=false) when
// the iterator leaves the contract's prefix range.
void
advance_iter(ContractIter &ci, IterationOrder order)
{
  if (!ci.iter->Valid()) {
    ci.has_current = false;
    return;
  }
  if (order == IterationOrder::IterationOrderAscending) {
    ci.iter->Next();
  }
  else {
    ci.iter->Prev();
  }
  if (!ci.iter->Valid()) {
    ci.has_current = false;
    return;
  }
  rocksdb::Slice key = ci.iter->key();
  // Verify we're still under this contract's prefix.
  if (key.size() < 32 || std::memcmp(key.data(), ci.contract.data(), 32) != 0) {
    ci.has_current = false;
    return;
  }
  ContractAddress addr_out;
  if (!parse_contract_key_full(std::string_view(key.data(), key.size()),
                               &addr_out, &ci.current_block,
                               &ci.current_tx_hash, &ci.current_seq)) {
    ci.has_current = false;
    return;
  }
  ci.has_current = true;
}

// Initial seek for an iterator, given the anchor block and order.
// `anchor_block` is treated as an inclusive starting boundary.
void
seek_iter(ContractIter &ci, BlockHeight anchor_block, IterationOrder order)
{
  std::string prefix;
  IndexerKeyFormat::make_contract_key_prefix(prefix, ci.contract, anchor_block);
  if (order == IterationOrder::IterationOrderAscending) {
    ci.iter->Seek(prefix);
  }
  else {
    // Descending: we want entries at block <= anchor_block.  The prefix
    // above lands at the first key with block == anchor_block; append
    // a trailing all-FF to push past it for SeekForPrev.
    std::string upper_prefix = prefix;
    upper_prefix.append(32 + 4, static_cast<char>(0xFF));
    ci.iter->SeekForPrev(upper_prefix);
  }
  if (!ci.iter->Valid()) {
    ci.has_current = false;
    return;
  }
  rocksdb::Slice key = ci.iter->key();
  if (key.size() < 32 || std::memcmp(key.data(), ci.contract.data(), 32) != 0) {
    ci.has_current = false;
    return;
  }
  ContractAddress addr_out;
  if (!parse_contract_key_full(std::string_view(key.data(), key.size()),
                               &addr_out, &ci.current_block,
                               &ci.current_tx_hash, &ci.current_seq)) {
    ci.has_current = false;
    return;
  }
  ci.has_current = true;
}

// Advance past the encoded cursor position (exclusive resume).
// Called after seek_iter(anchor) when the anchor is a cursor-encoded
// last-emitted position.  Skips any entries up to AND INCLUDING
// (cursor_block, cursor_tx, cursor_seq).
void
skip_past_cursor(ContractIter &ci, IterationOrder order,
                 BlockHeight cursor_block, SHA256Hash const &cursor_tx,
                 uint32_t cursor_seq)
{
  while (ci.has_current) {
    const bool at_or_past_cursor =
        (order == IterationOrder::IterationOrderAscending)
            ? !position_less(ci.current_block, ci.current_tx_hash,
                             ci.current_seq, cursor_block, cursor_tx,
                             cursor_seq)
            : !position_less(cursor_block, cursor_tx, cursor_seq,
                             ci.current_block, ci.current_tx_hash,
                             ci.current_seq);
    if (!at_or_past_cursor) {
      // Iterator is still behind the cursor in iteration order —
      // shouldn't happen after a correct seek, but handle defensively.
      advance_iter(ci, order);
      continue;
    }
    const bool exactly_at_cursor =
        ci.current_block == cursor_block &&
        std::memcmp(ci.current_tx_hash.data(), cursor_tx.data(), 32) == 0 &&
        ci.current_seq == cursor_seq;
    if (exactly_at_cursor) {
      advance_iter(ci, order);
    }
    // Either we're past the cursor already, or we just advanced past
    // an exact-match entry — in both cases the iterator now points at
    // the first entry strictly past the cursor.
    return;
  }
}

} // namespace

IndexerStatus<EventPage>
IndexDB::get_events(EventQuery const &query) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  EventPage page;
  page.latest_block = latest_block();
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
    order = decoded->order;
    cursor_block = decoded->block_height;
    cursor_tx = decoded->tx_hash;
    cursor_seq = decoded->sequence_number;
    have_cursor = true;
    anchor_block = cursor_block;
  }
  else {
    order = query.order;
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
      anchor_block = latest_block();
      break;
    case BlockAnchorKind::AnchorAtHeight:
      anchor_block = query.anchor.height();
      break;
    }
  }

  // Dedupe contracts across all filters.  For each unique contract,
  // remember which filter indices included it — we'll run topic
  // post-filter against each when a candidate surfaces.
  std::vector<ContractIter> iters;
  for (size_t fidx = 0; fidx < query.filters.size(); ++fidx) {
    for (ContractAddress const &addr : query.filters[fidx].contract_addresses) {
      // Look for an existing iterator for this contract.
      auto it = std::ranges::find_if(iters, [&](ContractIter const &ci) {
        return std::memcmp(ci.contract.data(), addr.data(), 32) == 0;
      });
      if (it != iters.end()) {
        it->filter_indices.push_back(fidx);
      }
      else {
        ContractIter ci;
        ci.contract = addr;
        ci.filter_indices.push_back(fidx);
        ci.iter.reset(
            db_->NewIterator(rocksdb::ReadOptions(), contract_cf_handle_));
        iters.push_back(std::move(ci));
      }
    }
  }

  // Seek each iterator to the anchor.
  for (ContractIter &ci : iters) {
    seek_iter(ci, anchor_block, order);
    if (have_cursor && ci.has_current) {
      skip_past_cursor(ci, order, cursor_block, cursor_tx, cursor_seq);
    }
  }

  // Merge-walk.  At each step, pick the iterator whose current
  // position is smallest (ascending) or largest (descending) and
  // consume it.  Stop at `limit` matches or exhaustion.
  BlockHeight last_emitted_block{0};
  SHA256Hash last_emitted_tx{};
  uint32_t last_emitted_seq = 0;
  bool any_emitted = false;

  while (page.events.size() < limit) {
    // Find the winner — the valid iterator whose current position
    // leads in the requested direction.
    size_t winner = iters.size();
    for (size_t i = 0; i < iters.size(); ++i) {
      if (!iters[i].has_current) {
        continue;
      }
      if (winner == iters.size()) {
        winner = i;
        continue;
      }
      const bool i_leads =
          (order == IterationOrder::IterationOrderAscending)
              ? position_less(iters[i].current_block, iters[i].current_tx_hash,
                              iters[i].current_seq, iters[winner].current_block,
                              iters[winner].current_tx_hash,
                              iters[winner].current_seq)
              : position_less(iters[winner].current_block,
                              iters[winner].current_tx_hash,
                              iters[winner].current_seq, iters[i].current_block,
                              iters[i].current_tx_hash, iters[i].current_seq);
      if (i_leads) {
        winner = i;
      }
    }
    if (winner == iters.size()) {
      break; // all exhausted
    }

    ContractIter &win = iters[winner];
    const BlockHeight block = win.current_block;
    const SHA256Hash tx_hash = win.current_tx_hash;
    const uint32_t seq = win.current_seq;

    // Hydrate.
    auto tx_events = get_tx_events(tx_hash);
    if (!tx_events) {
      error("get_events: tx_index miss for tx {} at block {}",
            HexArray(tx_hash).str(), static_cast<int64_t>(block));
      advance_iter(win, order);
      continue;
    }
    const TxEventLog &event_log = tx_events->second;
    if (seq >= event_log.events.size()) {
      error("get_events: seq {} out of range ({}) for tx {}", seq,
            event_log.events.size(), HexArray(tx_hash).str());
      advance_iter(win, order);
      continue;
    }
    TxEmittedEvent const &emitted_event = event_log.events[seq];

    // Topic filter: match if ANY filter that includes this contract
    // accepts the event.  One entry in the contract_index = one event
    // = one emission; since dedupe-by-contract is done above, we
    // can't double-emit.
    bool matched = false;
    for (size_t fidx : win.filter_indices) {
      if (event_topics_match(query.filters[fidx].topics, emitted_event)) {
        matched = true;
        break;
      }
    }

    if (matched) {
      IdentifiedEvent ev;
      ev.proposed_tx_hash = tx_hash;
      ev.block_height = block;
      ev.sequence_number = seq;
      ev.emitted_event = emitted_event;
      page.events.push_back(std::move(ev));
      last_emitted_block = block;
      last_emitted_tx = tx_hash;
      last_emitted_seq = seq;
      any_emitted = true;
    }

    advance_iter(win, order);
  }

  // Cursor for continuation: non-empty iff we emitted something AND
  // any iterator is still positioned at more data.  If all iterators
  // are exhausted, we return an empty cursor to signal end-of-stream.
  //
  // We signal empty cursor only when truly exhausted, to distinguish
  // "more-results-to-scan" (selective filter yielded no matches this
  // window) from "end of data".
  bool any_more = false;
  for (ContractIter const &ci : iters) {
    if (ci.has_current) {
      any_more = true;
      break;
    }
  }

  if (any_more) {
    if (any_emitted) {
      EventCursorPayload payload;
      payload.order = order;
      payload.block_height = last_emitted_block;
      payload.tx_hash = last_emitted_tx;
      payload.sequence_number = last_emitted_seq;
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
