#include "hogd/hog_indexer/db.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <optional>
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
#include "hogd/hog_indexer/event_cursor.h"
#include "hogd/hog_indexer/key_format.h"
#include "hogd/hog_indexer/status.h"
#include "hogd/hog_indexer/topic_layout.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "network.hh"
#include "pagination.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
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
    std::span<const ProposedTransaction> bodies_chunk,
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

  // [events_chunk] and [bodies_chunk] are index-aligned.  When no body is
  // available (e.g. tests that only exercise the event-log path) an empty
  // ProposedTransaction is stored.
  const ProposedTransaction empty_body{};

  for (size_t i = 0; i < events_chunk.size(); i++) {
    const TxEventLog &event_log = events_chunk[i];
    const ProposedTransaction &body =
        i < bodies_chunk.size() ? bodies_chunk[i] : empty_body;

    // Value layout: block_height || serialized ProposedTransaction (the tx
    // body) || serialized TxEventLog.  The body precedes the event log;
    // get_tx_events() skips it with deserialize_from().
    std::string value(reinterpret_cast<const char *>(&block_height),
                      sizeof(block_height));

    auto body_serialized = x23::serialize(body);
    value.append(reinterpret_cast<const char *>(body_serialized.data()),
                 body_serialized.size());

    auto serialized = x23::serialize(event_log);
    value.append(reinterpret_cast<const char *>(serialized.data()),
                 serialized.size());

    // Index by transaction (in tx_index column family)
    IndexerKeyFormat::make_tx_key(key_buffer, event_log.tx_hash);
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
    std::span<const ProposedTransaction> bodies_chunk,
    const std::string &sst_path, const rocksdb::Options &sst_options,
    const rocksdb::EnvOptions &env_options, size_t chunk_idx)
{
  // Count total events; reserve space for events + one ROOT_TX row per body.
  size_t total_events = 0;
  for (const auto &event_log : events_chunk) {
    total_events += event_log.events.size();
  }

  // Skip only when this chunk would produce no rows at all.
  if (total_events == 0 && bodies_chunk.empty()) {
    return false;
  }

  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(total_events + bodies_chunk.size());
  std::string key_buffer;
  const std::string empty_string;

  for (size_t i = 0; i < events_chunk.size(); i++) {
    const TxEventLog &event_log = events_chunk[i];

    uint32_t sequence_number = 0;
    for (const auto &emitted_event : event_log.events) {
      IndexerKeyFormat::make_contract_key(key_buffer,
                                          emitted_event.contract_address,
                                          IndexerKeyFormat::kEvent,
                                          block_height, event_log.tx_hash,
                                          sequence_number);
      entries.emplace_back(key_buffer, empty_string);
      sequence_number++;
    }

    // One ROOT_TX row keyed by the tx's root invoked address (seq = 0).
    // Bodies are index-aligned with events; callers that pass no bodies
    // (event-log-only tests) simply get no ROOT_TX rows.
    if (i < bodies_chunk.size()) {
      const ContractAddress &root_addr =
          bodies_chunk[i].signed_tx.tx.invocation.invoked_address;
      IndexerKeyFormat::make_contract_key(key_buffer, root_addr,
                                          IndexerKeyFormat::kRootTx,
                                          block_height, event_log.tx_hash, 0);
      entries.emplace_back(key_buffer, empty_string);
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

// Parse a tx_index value: block_height || ProposedTransaction || TxEventLog.
// The caller must ensure [value] is at least sizeof(BlockHeight) bytes.
// Throws x23::XdrBadInput if the body or event log is malformed.
IndexDB::TxRecord
parse_tx_value(std::string_view value)
{
  // TODO(jhumphri): Support different endianness.
  static_assert(std::endian::native == std::endian::little);
  CHECK_GE(value.size(), sizeof(BlockHeight));
  IndexDB::TxRecord record;
  // value.data() is not guaranteed to be 8-byte aligned, so we memcpy.
  std::memcpy(&record.height, value.data(), sizeof(BlockHeight));
  const auto *payload =
      reinterpret_cast<const uint8_t *>(value.data() + sizeof(BlockHeight));
  const size_t payload_len = value.size() - sizeof(BlockHeight);
  const size_t body_len =
      x23::deserialize_from(&record.tx, x23::BufView{payload, payload_len});
  x23::deserialize(&record.events,
                   x23::BufView{payload + body_len, payload_len - body_len});
  return record;
}

template<typename T>
bool
xdr_equal(const T &lhs, const T &rhs)
{
  return x23::serialize(lhs) == x23::serialize(rhs);
}

std::string
bounded_diagnostic(std::string message)
{
  constexpr size_t kMaxDiagnosticBytes = 1024;
  if (message.size() > kMaxDiagnosticBytes) {
    message.resize(kMaxDiagnosticBytes);
  }
  return message;
}

IndexerProgress
uninitialized_progress()
{
  return IndexerProgress(IndexerProgressState::IndexerProgressUninitialized);
}

IndexerProgress
initialized_progress(const IndexerInitializedProgress &progress)
{
  IndexerProgress result(IndexerProgressState::IndexerProgressInitialized);
  result.initialized() = progress;
  return result;
}

IndexerEpochResult
epoch_success(IndexerEpochOutcome outcome,
              const IndexerInitializedProgress &progress)
{
  IndexerEpochResult result(outcome);
  result.progress() = progress;
  return result;
}

IndexerEpochResult
epoch_failure(IndexerEpochOutcome outcome, std::string diagnostic)
{
  IndexerEpochResult result(outcome);
  diagnostic = bounded_diagnostic(std::move(diagnostic));
  result.diagnostic() =
      IndexerFailureMessage(diagnostic.data(), diagnostic.size());
  return result;
}

IndexerBlockApplyResult
apply_success(IndexerBlockApplyOutcome outcome,
              const IndexerInitializedProgress &progress)
{
  IndexerBlockApplyResult result(outcome);
  result.progress() = progress;
  return result;
}

IndexerBlockApplyResult
apply_failure(IndexerBlockApplyOutcome outcome, std::string diagnostic)
{
  IndexerBlockApplyResult result(outcome);
  diagnostic = bounded_diagnostic(std::move(diagnostic));
  result.diagnostic() =
      IndexerFailureMessage(diagnostic.data(), diagnostic.size());
  return result;
}

IndexerBlockApplyResult
apply_gap(BlockHeight expected_height)
{
  IndexerBlockApplyResult result(IndexerBlockApplyOutcome::IndexerBlockGap);
  result.gap().expected_height = expected_height;
  return result;
}

std::string
make_tx_value(BlockHeight block_height, const ProposedTransaction &body,
              const TxEventLog &event_log)
{
  static_assert(std::endian::native == std::endian::little);
  std::string value(reinterpret_cast<const char *>(&block_height),
                    sizeof(block_height));
  const x23::Buf body_bytes = x23::serialize(body);
  value.append(reinterpret_cast<const char *>(body_bytes.data()),
               body_bytes.size());
  const x23::Buf event_bytes = x23::serialize(event_log);
  value.append(reinterpret_cast<const char *>(event_bytes.data()),
               event_bytes.size());
  return value;
}

IndexerStatus<bool>
column_family_has_rows(rocksdb::DB *db, rocksdb::ColumnFamilyHandle *handle)
{
  std::unique_ptr<rocksdb::Iterator> iterator(
      db->NewIterator(rocksdb::ReadOptions(), handle));
  iterator->SeekToFirst();
  if (!iterator->status().ok()) {
    error("failed to inspect indexer column family {}: {}", handle->GetName(),
          iterator->status().ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }
  return iterator->Valid();
}

IndexerStatus<bool>
column_family_has_prefix(rocksdb::DB *db, rocksdb::ColumnFamilyHandle *handle,
                         std::string_view prefix)
{
  std::unique_ptr<rocksdb::Iterator> iterator(
      db->NewIterator(rocksdb::ReadOptions(), handle));
  iterator->Seek(rocksdb::Slice(prefix.data(), prefix.size()));
  if (!iterator->status().ok()) {
    error("failed to inspect indexer metadata prefix: {}",
          iterator->status().ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }
  return iterator->Valid() && iterator->key().starts_with(
                                  rocksdb::Slice(prefix.data(), prefix.size()));
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
  auto initialized = idx.initialize_progress_from_db();
  if (!initialized) {
    return initialized.error();
  }
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

IndexDB::IndexDB(IndexDB &&other) noexcept // NOLINT(bugprone-exception-escape)
  : db_(std::move(other.db_)), tx_cf_handle_(other.tx_cf_handle_),
    contract_cf_handle_(other.contract_cf_handle_),
    stats_worker_thread_(std::move(other.stats_worker_thread_)),
    pending_heights_(std::move(other.pending_heights_)),
    durable_progress_(std::move(other.durable_progress_)),
    legacy_mode_(other.legacy_mode_),
    legacy_ingest_test_hook_(std::move(other.legacy_ingest_test_hook_)),
    initialize_epoch_test_hook_(std::move(other.initialize_epoch_test_hook_)),
    apply_validation_test_hook_(std::move(other.apply_validation_test_hook_)),
    highest_indexed_block_(
        other.highest_indexed_block_.load(std::memory_order_acquire))
{
  other.tx_cf_handle_ = nullptr;
  other.contract_cf_handle_ = nullptr;
}

IndexDB &
// NOLINTNEXTLINE(bugprone-exception-escape)
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
    durable_progress_ = std::move(other.durable_progress_);
    legacy_mode_ = other.legacy_mode_;
    legacy_ingest_test_hook_ = std::move(other.legacy_ingest_test_hook_);
    initialize_epoch_test_hook_ = std::move(other.initialize_epoch_test_hook_);
    apply_validation_test_hook_ = std::move(other.apply_validation_test_hook_);
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
                        std::span<const TxEventLog> events,
                        std::span<const ProposedTransaction> bodies)
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  // Legacy ingest must reserve the state through both external-SST ingests and
  // the watermark write. Production uses durable strict ingestion; there is
  // intentionally no supported mixed legacy/strict deployment.
  std::unique_lock ingest_guard(ingest_mutex_);
  if (durable_progress_) {
    error("consume_events is disabled after durable epoch initialization");
    return IndexerError(IndexerErrorCode::kInternal);
  }
  if (height_seen(static_cast<int64_t>(block_height), ingest_guard)) {
    warn("consume_events: ignoring duplicate ingest for block {}",
         static_cast<int64_t>(block_height));
    return {};
  }
  if (legacy_ingest_test_hook_) {
    legacy_ingest_test_hook_();
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
    // Bodies are index-aligned with events; clamp to whatever bodies were
    // supplied (empty for callers that pass none).
    const auto bodies_chunk =
        start_idx < bodies.size()
            ? bodies.subspan(start_idx,
                             std::min(end_idx, bodies.size()) - start_idx)
            : std::span<const ProposedTransaction>{};
    const std::string &chunk_sst_path = tx_sst_paths[i];

    auto *db_ptr = db_.get();
    CHECK_PTR(db_ptr);

    auto future = pool.submit([db = db_ptr, tx_cf = tx_cf_handle_, block_height,
                               events_chunk, bodies_chunk, &chunk_sst_path,
                               &tx_sst_options, &env_options,
                               i]() -> IndexerStatus<bool> {
      return create_tx_index_sst_chunk(db, tx_cf, block_height, events_chunk,
                                       bodies_chunk, chunk_sst_path,
                                       tx_sst_options, env_options, i);
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
    const auto bodies_chunk =
        start_idx < bodies.size()
            ? bodies.subspan(start_idx,
                             std::min(end_idx, bodies.size()) - start_idx)
            : std::span<const ProposedTransaction>{};
    const std::string &chunk_sst_path = contract_sst_paths[i];

    auto *db_ptr = db_.get();
    CHECK_PTR(db_ptr);

    auto future = pool.submit([db = db_ptr, contract_cf = contract_cf_handle_,
                               block_height, events_chunk, bodies_chunk,
                               &chunk_sst_path, &contract_sst_options,
                               &env_options, i]() -> IndexerStatus<bool> {
      try {
        return create_contract_index_sst_chunk(db, contract_cf, block_height,
                                               events_chunk, bodies_chunk,
                                               chunk_sst_path,
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
  record_height(static_cast<int64_t>(block_height), ingest_guard);
  legacy_mode_ = true;

  auto end_ts = x23::fastclock::get_ns();
  info("consume_events time: {} ns ({:.3f} s)", end_ts - start_ts,
       (end_ts - start_ts) / 1'000'000'000.0);

  return {};
}

bool
IndexDB::height_seen(int64_t h,
                     std::unique_lock<std::mutex> const & /*g*/) const
{
  const int64_t latest = highest_indexed_block_.load(std::memory_order_relaxed);
  return (latest != kNoBlocksIndexed && h <= latest) ||
         pending_heights_.contains(h);
}

void
IndexDB::record_height(int64_t h, std::unique_lock<std::mutex> const &g)
{
  // consume_events holds this lock from duplicate detection through this
  // update.
  CHECK(!height_seen(h, g));
  int64_t latest = highest_indexed_block_.load(std::memory_order_relaxed);
  if (latest != kNoBlocksIndexed && h != latest + 1) {
    // Out-of-order arrival above a gap: ingested, but not servable
    // until the gap fills.  Expected to be rare — commit-time fanout
    // delivers heights in order.  No size cap: dropping entries
    // would silently lose blocks; the logged set size is the
    // operator's signal that a gap is stuck.
    warn("consume_events: out-of-order block {} buffered (watermark {}, "
         "{} pending)",
         h, latest, pending_heights_.size() + 1);
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

IndexerProgress
IndexDB::durable_progress() const
{
  std::lock_guard g(ingest_mutex_);
  if (!durable_progress_) {
    return uninitialized_progress();
  }
  return initialized_progress(*durable_progress_);
}

IndexerEpochResult
IndexDB::initialize_epoch(const IndexerOutboxOrigin &origin)
{
  std::lock_guard g(ingest_mutex_);
  if (initialize_epoch_test_hook_) {
    initialize_epoch_test_hook_();
  }
  if (!is_open()) {
    return epoch_failure(IndexerEpochOutcome::IndexerEpochInternalFailure,
                         "indexer database is not open");
  }
  if (origin.protocol.empty() || origin.first_height < 0) {
    return epoch_failure(IndexerEpochOutcome::IndexerEpochConflict,
                         "invalid outbox epoch origin");
  }

  if (durable_progress_) {
    if (xdr_equal(durable_progress_->origin, origin)) {
      return epoch_success(IndexerEpochOutcome::IndexerEpochUnchanged,
                           *durable_progress_);
    }
    if (durable_progress_->origin.epoch_sequence == UINT64_MAX ||
        origin.epoch_sequence != durable_progress_->origin.epoch_sequence + 1) {
      return epoch_failure(
          IndexerEpochOutcome::IndexerEpochConflict,
          "outbox epoch sequence is not the next durable sequence");
    }
    const bool current_epoch_empty = durable_progress_->contiguous_height ==
                                     durable_progress_->origin.first_height - 1;
    const BlockHeight expected_first_height =
        current_epoch_empty ? durable_progress_->origin.first_height
                            : durable_progress_->contiguous_height + 1;
    if (xdr_equal(durable_progress_->origin.protocol, origin.protocol) ||
        (!current_epoch_empty &&
         durable_progress_->contiguous_height == INT64_MAX) ||
        origin.first_height != expected_first_height ||
        origin.first_prev_hash != durable_progress_->last_consensus_hash) {
      return epoch_failure(
          IndexerEpochOutcome::IndexerEpochConflict,
          "outbox epoch does not continue the durable indexer boundary");
    }
  }
  else {
    if (legacy_mode_) {
      return epoch_failure(IndexerEpochOutcome::IndexerEpochConflict,
                           "legacy index data requires explicit migration");
    }
    if (origin.epoch_sequence != 0 ||
        origin.first_height != kFirstOutboxBlockHeight ||
        origin.first_prev_hash == ConsensusValueHash{}) {
      return epoch_failure(
          IndexerEpochOutcome::IndexerEpochConflict,
          "fresh outbox origin must begin at height 1 and link genesis");
    }
    auto default_has_rows =
        column_family_has_rows(db_.get(), db_->DefaultColumnFamily());
    if (!default_has_rows) {
      return epoch_failure(IndexerEpochOutcome::IndexerEpochInternalFailure,
                           "failed to inspect index metadata");
    }
    if (*default_has_rows) {
      return epoch_failure(IndexerEpochOutcome::IndexerEpochConflict,
                           "legacy index data requires explicit migration");
    }
    for (rocksdb::ColumnFamilyHandle *handle :
         {tx_cf_handle_, contract_cf_handle_}) {
      auto has_rows = column_family_has_rows(db_.get(), handle);
      if (!has_rows) {
        return epoch_failure(IndexerEpochOutcome::IndexerEpochInternalFailure,
                             "failed to inspect legacy index data");
      }
      if (*has_rows) {
        return epoch_failure(IndexerEpochOutcome::IndexerEpochConflict,
                             "legacy index data requires explicit migration");
      }
    }
    if (highest_indexed_block_.load(std::memory_order_relaxed) !=
            kNoBlocksIndexed ||
        !pending_heights_.empty()) {
      return epoch_failure(IndexerEpochOutcome::IndexerEpochConflict,
                           "legacy index data requires explicit migration");
    }
  }

  const std::string epoch_key =
      IndexerKeyFormat::make_epoch_history_meta_key(origin.protocol);
  std::string prior_epoch;
  const rocksdb::Status epoch_status =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), epoch_key,
               &prior_epoch);
  if (epoch_status.ok()) {
    return epoch_failure(IndexerEpochOutcome::IndexerEpochConflict,
                         "outbox protocol epoch was already used");
  }
  if (!epoch_status.IsNotFound()) {
    error("failed to inspect outbox epoch history: {}",
          epoch_status.ToString());
    return epoch_failure(IndexerEpochOutcome::IndexerEpochInternalFailure,
                         "failed to inspect outbox epoch history");
  }

  IndexerInitializedProgress progress{
      .origin = origin,
      .contiguous_height = origin.first_height - 1,
      .last_consensus_hash = origin.first_prev_hash,
  };
  if (durable_progress_) {
    progress.contiguous_height = durable_progress_->contiguous_height;
    progress.last_consensus_hash = durable_progress_->last_consensus_hash;
  }

  const x23::Buf progress_bytes = x23::serialize(progress);
  const x23::Buf origin_bytes = x23::serialize(origin);
  const uint64_t encoded_watermark =
      std::byteswap(static_cast<uint64_t>(progress.contiguous_height));
  rocksdb::WriteBatch batch;
  batch.Put(db_->DefaultColumnFamily(),
            rocksdb::Slice(IndexerKeyFormat::kDurableProgressMetaKey),
            rocksdb::Slice(reinterpret_cast<const char *>(
                               progress_bytes.data()),
                           progress_bytes.size()));
  batch.Put(db_->DefaultColumnFamily(),
            rocksdb::Slice(IndexerKeyFormat::kHighestIndexedBlockMetaKey),
            rocksdb::Slice(reinterpret_cast<const char *>(&encoded_watermark),
                           sizeof(encoded_watermark)));
  batch.Put(db_->DefaultColumnFamily(), epoch_key,
            rocksdb::Slice(reinterpret_cast<const char *>(origin_bytes.data()),
                           origin_bytes.size()));
  rocksdb::WriteOptions options;
  options.sync = true;
  const rocksdb::Status status = db_->Write(options, &batch);
  if (!status.ok()) {
    error("failed to initialize durable indexer epoch: {}", status.ToString());
    return epoch_failure(IndexerEpochOutcome::IndexerEpochInternalFailure,
                         "failed to persist outbox epoch");
  }

  durable_progress_ = progress;
  highest_indexed_block_.store(progress.contiguous_height,
                               std::memory_order_release);
  return epoch_success(IndexerEpochOutcome::IndexerEpochInitialized, progress);
}

x23::Async<IndexerBlockApplyResult>
IndexDB::apply_committed_block(IndexerOutboxOrigin origin,
                               IndexerCommittedBlock block)
{
  {
    std::lock_guard g(ingest_mutex_);
    if (!is_open()) {
      co_return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
          "indexer database is not open");
    }
    if (!durable_progress_ || !xdr_equal(durable_progress_->origin, origin)) {
      co_return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockOriginMismatch,
          durable_progress_ ? "outbox origin mismatch"
                            : "outbox epoch is uninitialized");
    }
  }
  if (apply_validation_test_hook_) {
    apply_validation_test_hook_();
  }
  try {
    if (auto valid = co_await validate_indexer_committed_block(block); !valid) {
      co_return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockContentConflict, valid.error());
    }
    co_return apply_validated_committed_block(origin, block);
  } catch (const std::exception &e) {
    error("failed to validate/apply committed indexer block: {}", e.what());
    co_return apply_failure(
        IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
        "internal committed block failure");
  }
}

IndexerBlockApplyResult
IndexDB::apply_validated_committed_block(const IndexerOutboxOrigin &origin,
                                         const IndexerCommittedBlock &block)
{
  std::lock_guard g(ingest_mutex_);
  if (!is_open()) {
    return apply_failure(IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
                         "indexer database is not open");
  }
  if (!durable_progress_ || !xdr_equal(durable_progress_->origin, origin)) {
    return apply_failure(IndexerBlockApplyOutcome::IndexerBlockOriginMismatch,
                         durable_progress_ ? "outbox origin mismatch"
                                           : "outbox epoch is uninitialized");
  }

  try {
    const BlockHeight height =
        block.payload.consensus_value.proposal_header.block_height;
    if (height < 0) {
      return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockContentConflict,
          "committed block height is negative");
    }
    if (height < durable_progress_->origin.first_height) {
      return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockContentConflict,
          "committed block predates the current outbox epoch");
    }

    if (height <= durable_progress_->contiguous_height) {
      std::string persisted_bytes;
      const rocksdb::Status status =
          db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
                   IndexerKeyFormat::make_block_identity_meta_key(height),
                   &persisted_bytes);
      if (status.IsNotFound()) {
        return apply_failure(
            IndexerBlockApplyOutcome::IndexerBlockContentConflict,
            "no durable identity exists for the older height");
      }
      if (!status.ok()) {
        error("failed to read durable block identity at {}: {}", height,
              status.ToString());
        return apply_failure(
            IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
            "failed to read durable block identity");
      }
      IndexerCommittedBlockIdentity persisted;
      try {
        x23::deserialize(&persisted,
                         x23::BufView(reinterpret_cast<const uint8_t *>(
                                          persisted_bytes.data()),
                                      persisted_bytes.size()));
      } catch (const x23::XdrBadInput &) {
        return apply_failure(
            IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
            "stored durable block identity is malformed");
      }
      if (xdr_equal(persisted, block.identity)) {
        return apply_success(IndexerBlockApplyOutcome::IndexerBlockDuplicate,
                             *durable_progress_);
      }
      return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockContentConflict,
          "committed block conflicts with the durable identity at its height");
    }

    if (durable_progress_->contiguous_height == INT64_MAX) {
      return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockContentConflict,
          "durable indexer height is exhausted");
    }
    const BlockHeight expected_height =
        durable_progress_->contiguous_height + 1;
    if (height != expected_height) {
      return apply_gap(expected_height);
    }
    if (block.payload.consensus_value.prev_hash !=
        durable_progress_->last_consensus_hash) {
      return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockContentConflict,
          "committed block previous hash does not match durable progress");
    }

    const auto &events = block.payload.event_logs;
    const auto &bodies = block.payload.tx_bodies;
    rocksdb::WriteBatch batch;
    std::string key;
    for (size_t i = 0; i < events.size(); ++i) {
      const TxEventLog &event_log = events[i];
      const ProposedTransaction &body = bodies[i];
      IndexerKeyFormat::make_tx_key(key, event_log.tx_hash);

      std::string existing;
      const rocksdb::Status existing_status =
          db_->Get(rocksdb::ReadOptions(), tx_cf_handle_, key, &existing);
      if (!existing_status.IsNotFound()) {
        if (!existing_status.ok()) {
          error("failed to check existing transaction row: {}",
                existing_status.ToString());
          return apply_failure(
              IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
              "failed to inspect transaction index");
        }
        return apply_failure(
            IndexerBlockApplyOutcome::IndexerBlockContentConflict,
            "committed transaction hash already exists at another height");
      }

      batch.Put(tx_cf_handle_, key, make_tx_value(height, body, event_log));
      uint32_t sequence_number = 0;
      for (const TxEmittedEvent &emitted_event : event_log.events) {
        IndexerKeyFormat::make_contract_key(key, emitted_event.contract_address,
                                            IndexerKeyFormat::kEvent, height,
                                            event_log.tx_hash,
                                            sequence_number++);
        batch.Put(contract_cf_handle_, key, rocksdb::Slice());
      }
      IndexerKeyFormat::make_contract_key(
          key, body.signed_tx.tx.invocation.invoked_address,
          IndexerKeyFormat::kRootTx, height, event_log.tx_hash, 0);
      batch.Put(contract_cf_handle_, key, rocksdb::Slice());
    }

    IndexerInitializedProgress progress = *durable_progress_;
    progress.contiguous_height = height;
    progress.last_consensus_hash = block.identity.consensus_hash;
    const x23::Buf identity_bytes = x23::serialize(block.identity);
    const x23::Buf progress_bytes = x23::serialize(progress);
    const uint64_t encoded_watermark =
        std::byteswap(static_cast<uint64_t>(height));
    batch.Put(db_->DefaultColumnFamily(),
              IndexerKeyFormat::make_block_identity_meta_key(height),
              rocksdb::Slice(reinterpret_cast<const char *>(
                                 identity_bytes.data()),
                             identity_bytes.size()));
    batch.Put(db_->DefaultColumnFamily(),
              rocksdb::Slice(IndexerKeyFormat::kDurableProgressMetaKey),
              rocksdb::Slice(reinterpret_cast<const char *>(
                                 progress_bytes.data()),
                             progress_bytes.size()));
    batch.Put(db_->DefaultColumnFamily(),
              rocksdb::Slice(IndexerKeyFormat::kHighestIndexedBlockMetaKey),
              rocksdb::Slice(reinterpret_cast<const char *>(&encoded_watermark),
                             sizeof(encoded_watermark)));

    rocksdb::WriteOptions options;
    options.sync = true;
    const rocksdb::Status status = db_->Write(options, &batch);
    if (!status.ok()) {
      error("failed to atomically apply committed indexer block {}: {}", height,
            status.ToString());
      return apply_failure(
          IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
          "failed to persist committed block");
    }

    durable_progress_ = progress;
    highest_indexed_block_.store(height, std::memory_order_release);
    return apply_success(IndexerBlockApplyOutcome::IndexerBlockApplied,
                         progress);
  } catch (const std::exception &e) {
    error("failed to validate/apply committed indexer block: {}", e.what());
    return apply_failure(IndexerBlockApplyOutcome::IndexerBlockInternalFailure,
                         "internal committed block failure");
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

IndexerStatus<IndexDB::TxRecord>
IndexDB::get_tx_record(SHA256Hash const &tx_hash) const
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

  try {
    TxRecord record = parse_tx_value(value);
    const int64_t watermark =
        highest_indexed_block_.load(std::memory_order_acquire);
    if (watermark == kNoBlocksIndexed || record.height > watermark) {
      return IndexerError(IndexerErrorCode::kDBNotFound);
    }
    return record;
  } catch (const x23::XdrBadInput &e) {
    error("Failed to deserialize tx_index value: {}", e.what());
    return IndexerError(IndexerErrorCode::kInternal);
  }
}

IndexerStatus<std::pair<BlockHeight, TxEventLog>>
IndexDB::get_tx_events(SHA256Hash const &tx_hash) const
{
  auto record = get_tx_record(tx_hash);
  if (!record) {
    return record.error();
  }
  return std::make_pair(record->height, std::move(record->events));
}

IndexerStatus<void>
IndexDB::initialize_progress_from_db()
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  std::string progress_bytes;
  rocksdb::Status progress_status =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
               rocksdb::Slice(IndexerKeyFormat::kDurableProgressMetaKey),
               &progress_bytes);
  if (!progress_status.ok() && !progress_status.IsNotFound()) {
    error("failed to read durable indexer progress: {}",
          progress_status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  std::string legacy_watermark;
  rocksdb::Status legacy_status =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
               rocksdb::Slice(IndexerKeyFormat::kHighestIndexedBlockMetaKey),
               &legacy_watermark);
  if (!legacy_status.ok() && !legacy_status.IsNotFound()) {
    error("failed to read legacy indexer watermark: {}",
          legacy_status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  if (progress_status.IsNotFound()) {
    if (legacy_status.ok()) {
      if (legacy_watermark.size() != sizeof(uint64_t)) {
        error("legacy indexer watermark is malformed");
        return IndexerError(IndexerErrorCode::kInternal);
      }
      uint64_t encoded = 0;
      std::memcpy(&encoded, legacy_watermark.data(), sizeof(encoded));
      const auto watermark = static_cast<int64_t>(std::byteswap(encoded));
      if (watermark < 0) {
        error("legacy indexer watermark is negative");
        return IndexerError(IndexerErrorCode::kInternal);
      }
      auto has_strict_metadata =
          column_family_has_prefix(db_.get(), db_->DefaultColumnFamily(),
                                   IndexerKeyFormat::kStrictMetadataPrefix);
      if (!has_strict_metadata) {
        return has_strict_metadata.error();
      }
      if (*has_strict_metadata) {
        error("legacy indexer watermark is mixed with strict metadata; "
              "explicit migration is required");
        return IndexerError(IndexerErrorCode::kInternal);
      }
      highest_indexed_block_.store(watermark, std::memory_order_release);
      legacy_mode_ = true;
      return {};
    }
    auto default_has_rows =
        column_family_has_rows(db_.get(), db_->DefaultColumnFamily());
    if (!default_has_rows) {
      return default_has_rows.error();
    }
    if (*default_has_rows) {
      error("indexer database has metadata but no durable progress; explicit "
            "migration is required");
      return IndexerError(IndexerErrorCode::kInternal);
    }
    for (rocksdb::ColumnFamilyHandle *handle :
         {tx_cf_handle_, contract_cf_handle_}) {
      auto has_rows = column_family_has_rows(db_.get(), handle);
      if (!has_rows) {
        return has_rows.error();
      }
      if (*has_rows) {
        error("indexer database has rows or metadata but no durable progress; "
              "explicit migration is required");
        return IndexerError(IndexerErrorCode::kInternal);
      }
    }
    return {};
  }

  IndexerInitializedProgress progress;
  try {
    x23::deserialize(&progress, x23::BufView(reinterpret_cast<const uint8_t *>(
                                                 progress_bytes.data()),
                                             progress_bytes.size()));
  } catch (const x23::XdrBadInput &e) {
    error("malformed durable indexer progress: {}", e.what());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  if (progress.origin.protocol.empty() || progress.origin.first_height < 0 ||
      progress.contiguous_height < progress.origin.first_height - 1) {
    error("invalid durable indexer progress bounds");
    return IndexerError(IndexerErrorCode::kInternal);
  }

  if (legacy_status.IsNotFound() ||
      legacy_watermark.size() != sizeof(uint64_t)) {
    error("durable indexer progress has missing or malformed watermark");
    return IndexerError(IndexerErrorCode::kInternal);
  }
  uint64_t encoded = 0;
  std::memcpy(&encoded, legacy_watermark.data(), sizeof(encoded));
  const auto watermark = static_cast<int64_t>(std::byteswap(encoded));
  if (watermark != progress.contiguous_height) {
    error("durable indexer progress and watermark disagree");
    return IndexerError(IndexerErrorCode::kInternal);
  }

  std::string epoch_bytes;
  const rocksdb::Status epoch_status =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
               IndexerKeyFormat::make_epoch_history_meta_key(
                   progress.origin.protocol),
               &epoch_bytes);
  if (!epoch_status.ok()) {
    error("durable current epoch history marker is unavailable: {}",
          epoch_status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }
  IndexerOutboxOrigin persisted_origin;
  try {
    x23::deserialize(&persisted_origin,
                     x23::BufView(reinterpret_cast<const uint8_t *>(
                                      epoch_bytes.data()),
                                  epoch_bytes.size()));
  } catch (const x23::XdrBadInput &e) {
    error("malformed durable current epoch history marker: {}", e.what());
    return IndexerError(IndexerErrorCode::kInternal);
  }
  if (!xdr_equal(persisted_origin, progress.origin)) {
    error("durable current epoch history marker disagrees with progress");
    return IndexerError(IndexerErrorCode::kInternal);
  }

  if (progress.contiguous_height >= progress.origin.first_height) {
    std::string identity_bytes;
    rocksdb::Status identity_status =
        db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
                 IndexerKeyFormat::make_block_identity_meta_key(
                     progress.contiguous_height),
                 &identity_bytes);
    if (!identity_status.ok()) {
      error("durable indexer boundary identity is unavailable: {}",
            identity_status.ToString());
      return IndexerError(IndexerErrorCode::kInternal);
    }
    IndexerCommittedBlockIdentity identity;
    try {
      x23::deserialize(&identity,
                       x23::BufView(reinterpret_cast<const uint8_t *>(
                                        identity_bytes.data()),
                                    identity_bytes.size()));
    } catch (const x23::XdrBadInput &e) {
      error("malformed durable indexer boundary identity: {}", e.what());
      return IndexerError(IndexerErrorCode::kInternal);
    }
    if (identity.consensus_hash != progress.last_consensus_hash) {
      error("durable indexer boundary identity disagrees with progress");
      return IndexerError(IndexerErrorCode::kInternal);
    }
  }
  else {
    if (progress.contiguous_height != progress.origin.first_height - 1 ||
        progress.last_consensus_hash != progress.origin.first_prev_hash) {
      error("unapplied durable indexer epoch has invalid boundary identity");
      return IndexerError(IndexerErrorCode::kInternal);
    }
    if (progress.contiguous_height == 0) {
      if (progress.origin.first_height != kFirstOutboxBlockHeight ||
          progress.origin.first_prev_hash == ConsensusValueHash{}) {
        error("fresh durable indexer epoch has invalid genesis boundary");
        return IndexerError(IndexerErrorCode::kInternal);
      }
    }
    else {
      std::string identity_bytes;
      const rocksdb::Status identity_status =
          db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
                   IndexerKeyFormat::make_block_identity_meta_key(
                       progress.contiguous_height),
                   &identity_bytes);
      if (!identity_status.ok()) {
        error("prior epoch boundary identity is unavailable: {}",
              identity_status.ToString());
        return IndexerError(IndexerErrorCode::kInternal);
      }
      IndexerCommittedBlockIdentity identity;
      try {
        x23::deserialize(&identity,
                         x23::BufView(reinterpret_cast<const uint8_t *>(
                                          identity_bytes.data()),
                                      identity_bytes.size()));
      } catch (const x23::XdrBadInput &e) {
        error("malformed prior epoch boundary identity: {}", e.what());
        return IndexerError(IndexerErrorCode::kInternal);
      }
      if (identity.consensus_hash != progress.last_consensus_hash) {
        error("prior epoch boundary identity disagrees with progress");
        return IndexerError(IndexerErrorCode::kInternal);
      }
    }
  }

  durable_progress_ = std::move(progress);
  highest_indexed_block_.store(watermark, std::memory_order_release);
  return {};
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
               BlockHeight max_block, IndexerKeyFormat::RecordKind kind)
    : contract_(addr), order_(order), max_block_(max_block), kind_(kind),
      iter_(db->NewIterator(rocksdb::ReadOptions(), cf))
  {
    // Boundary prefix: address(32) + kind(1).  refresh_current() stops the
    // walk when a key leaves this range, so an EVENT iterator never bleeds
    // into ROOT_TX rows for the same address (and vice versa).
    kind_prefix_.append(reinterpret_cast<const char *>(contract_.data()), 32);
    kind_prefix_.push_back(static_cast<char>(kind_));
  }

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
    IndexerKeyFormat::make_contract_key_prefix(prefix, contract_, kind_,
                                               anchor);
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
        !key_sv.starts_with(kind_prefix_)) {
      has_current_ = false;
      return;
    }
    auto parsed = IndexerKeyFormat::parse_contract_key(key_sv);
    if (!parsed) {
      has_current_ = false;
      return;
    }
    current_.block = parsed->block_height;
    current_.tx = parsed->tx_hash;
    current_.seq = parsed->sequence_number;
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
  IndexerKeyFormat::RecordKind kind_;
  std::string kind_prefix_;
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
  BlockHeight max_serve_block = latest_block();
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
    max_serve_block = std::min(max_serve_block, decoded->max_block_height);
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
                           max_serve_block, IndexerKeyFormat::kEvent);
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
    // Index-integrity: a contract_index row and the tx_index receipt it
    // hydrates through are written by the same consume_events() call, so
    // the row's (block, contract) must agree with the receipt.  The join
    // between the two column families is by tx hash alone -- tx_index
    // carries no receipt identity or event-vector digest -- so a second
    // write under that hash silently re-points every contract_index row
    // referencing it, and a contract-history query would return one
    // contract's payload under another contract's filter (#2295).
    //
    // Queries are read-only, so no caller can drive the index into this
    // state; reaching it means the on-disk index is already corrupt, and
    // serving corrupt history is worse than dying loudly.
    CHECK_EQ(tx_events->first, win_pos.block);

    const TxEventLog &event_log = tx_events->second;
    if (win_pos.seq >= event_log.events.size()) {
      error("get_events: seq {} out of range ({}) for tx {}", win_pos.seq,
            event_log.events.size(), HexArray(win_pos.tx).str());
      winner->advance(order);
      continue;
    }
    TxEmittedEvent const &emitted_event = event_log.events[win_pos.seq];

    // The row was created from this very event (see
    // create_contract_index_sst_chunk), and iterators are bounded to a
    // single (address, kind) prefix, so the emitter address must match
    // the contract whose stream we are walking.
    CHECK_EQ(emitted_event.contract_address, winner->contract());

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
      ev.tx_hash = win_pos.tx;
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
      payload.max_block_height = max_serve_block;
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

IndexerStatus<TransactionPage>
IndexDB::get_transactions(TransactionQuery const &query) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  TransactionPage page;
  BlockHeight max_serve_block = latest_block();
  page.latest_block = max_serve_block;
  page.oldest_block = oldest_block();

  const uint32_t limit = IndexDB::clamp_events_limit(query.limit);

  IterationOrder order;
  BlockHeight anchor_block;
  bool have_cursor = false;
  Position cursor_pos{};

  if (!query.cursor.empty()) {
    auto decoded = cursor::decode(
        std::string_view(reinterpret_cast<const char *>(query.cursor.data()),
                         query.cursor.size()));
    if (!decoded || !is_valid_iteration_order(decoded->order)) {
      return page;
    }
    order = decoded->order;
    max_serve_block = std::min(max_serve_block, decoded->max_block_height);
    cursor_pos.block = decoded->block_height;
    cursor_pos.tx = decoded->tx_hash;
    cursor_pos.seq = 0;
    anchor_block = cursor_pos.block;
    have_cursor = true;
  }

  else {
    order = query.order;
    if (!is_valid_iteration_order(order)) {
      return page;
    }
    switch (query.anchor.kind()) {
    case BlockAnchorKind::AnchorAtBeginning:
      if (order == IterationOrder::IterationOrderDescending) {
        return page;
      }
      anchor_block = oldest_block();
      break;
    case BlockAnchorKind::AnchorAtEnd:
      if (order == IterationOrder::IterationOrderAscending) {
        return page;
      }
      anchor_block = max_serve_block;
      break;
    case BlockAnchorKind::AnchorAtHeight:
      anchor_block = query.anchor.height();
      if (anchor_block < oldest_block()) {
        if (order == IterationOrder::IterationOrderDescending) {
          return page;
        }
        anchor_block = oldest_block();
      }
      break;
    }
  }
  if (order == IterationOrder::IterationOrderDescending &&
      anchor_block > max_serve_block) {
    anchor_block = max_serve_block;
  }

  struct TransactionRow {
    Position pos;
    ProposedTransaction tx;
    TxEventLog events;
  };
  std::vector<TransactionRow> rows;

  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> iter(
      db_->NewIterator(read_options, tx_cf_handle_));
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    rocksdb::Slice key = iter->key();
    if (key.size() != IndexerKeyFormat::kTxKeySize) {
      error("get_transactions: unexpected tx_index key size {}", key.size());
      return IndexerError(IndexerErrorCode::kInternal);
    }

    rocksdb::Slice value = iter->value();
    if (value.size() < sizeof(BlockHeight)) {
      error("get_transactions: short tx_index value for key size {}",
            key.size());
      return IndexerError(IndexerErrorCode::kInternal);
    }

    BlockHeight block_height;
    std::memcpy(&block_height, value.data(), sizeof(BlockHeight));
    if (block_height < oldest_block() || block_height > max_serve_block) {
      continue;
    }

    Position pos{.block = block_height, .tx = {}, .seq = 0};
    std::memcpy(pos.tx.data(), key.data(), pos.tx.size());

    if (have_cursor) {
      if (order == IterationOrder::IterationOrderAscending &&
          !(pos > cursor_pos)) {
        continue;
      }
      if (order == IterationOrder::IterationOrderDescending &&
          !(pos < cursor_pos)) {
        continue;
      }
    }
    else if (order == IterationOrder::IterationOrderAscending) {
      if (pos.block < anchor_block) {
        continue;
      }
    }
    else if (pos.block > anchor_block) {
      continue;
    }

    TxRecord record;
    try {
      record = parse_tx_value(std::string_view{value.data(), value.size()});
    } catch (const x23::XdrBadInput &e) {
      error("get_transactions: failed to deserialize tx_index value: {}",
            e.what());
      return IndexerError(IndexerErrorCode::kInternal);
    }

    rows.push_back(TransactionRow{.pos = pos,
                                  .tx = std::move(record.tx),
                                  .events = std::move(record.events)});
  }

  rocksdb::Status status = iter->status();
  if (!status.ok()) {
    error("get_transactions: tx_index iterator failed: {}", status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  std::ranges::sort(rows, [order](TransactionRow const &lhs,
                                  TransactionRow const &rhs) {
    return order == IterationOrder::IterationOrderAscending ? lhs.pos < rhs.pos
                                                            : lhs.pos > rhs.pos;
  });

  page.transactions.reserve(std::min<std::size_t>(limit, rows.size()));
  Position last_emitted{};
  bool any_emitted = false;
  for (TransactionRow &row : rows) {
    if (page.transactions.size() >= limit) {
      break;
    }
    TxInfo info{.height = row.pos.block,
                .tx = std::move(row.tx),
                .events = std::move(row.events)};
    page.transactions.push_back(std::move(info));
    last_emitted = row.pos;
    any_emitted = true;
  }

  if (any_emitted && page.transactions.size() < rows.size()) {
    EventCursorPayload payload;
    payload.order = order;
    payload.block_height = last_emitted.block;
    payload.tx_hash = last_emitted.tx;
    payload.sequence_number = 0;
    payload.max_block_height = max_serve_block;
    std::string encoded = cursor::encode(payload);
    page.cursor.append_range(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size()));
  }

  return page;
}

IndexerStatus<TransactionRefPage>
IndexDB::get_transactions_by_address(AddressTransactionQuery const &query) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  TransactionRefPage page;
  BlockHeight max_serve_block = latest_block();
  page.latest_block = max_serve_block;
  page.oldest_block = oldest_block();

  const uint32_t limit = IndexDB::clamp_events_limit(query.limit);

  IterationOrder order;
  BlockHeight anchor_block;
  bool have_cursor = false;
  Position cursor_pos{};

  if (!query.cursor.empty()) {
    auto decoded = cursor::decode(
        std::string_view(reinterpret_cast<const char *>(query.cursor.data()),
                         query.cursor.size()));
    if (!decoded || !is_valid_iteration_order(decoded->order)) {
      return page;
    }
    order = decoded->order;
    max_serve_block = std::min(max_serve_block, decoded->max_block_height);
    cursor_pos.block = decoded->block_height;
    cursor_pos.tx = decoded->tx_hash;
    // ROOT_TX rows are always written with seq == 0, so the position we
    // resume past never carries a meaningful sequence number.
    cursor_pos.seq = 0;
    anchor_block = cursor_pos.block;
    have_cursor = true;
  }

  else {
    order = query.order;
    if (!is_valid_iteration_order(order)) {
      return page;
    }
    switch (query.anchor.kind()) {
    case BlockAnchorKind::AnchorAtBeginning:
      if (order == IterationOrder::IterationOrderDescending) {
        return page;
      }
      anchor_block = oldest_block();
      break;
    case BlockAnchorKind::AnchorAtEnd:
      if (order == IterationOrder::IterationOrderAscending) {
        return page;
      }
      anchor_block = max_serve_block;
      break;
    case BlockAnchorKind::AnchorAtHeight:
      anchor_block = query.anchor.height();
      if (anchor_block < oldest_block()) {
        if (order == IterationOrder::IterationOrderDescending) {
          return page;
        }
        anchor_block = oldest_block();
      }
      break;
    }
  }
  if (order == IterationOrder::IterationOrderDescending &&
      anchor_block > max_serve_block) {
    anchor_block = max_serve_block;
  }

  ContractIter iter(db_.get(), contract_cf_handle_, query.address, order,
                    max_serve_block, IndexerKeyFormat::kRootTx);
  iter.seek(anchor_block, order);
  if (have_cursor && iter.has_current()) {
    iter.skip_past(order, cursor_pos);
  }

  Position last_emitted{};
  bool any_emitted = false;
  while (page.refs.size() < limit && iter.has_current()) {
    Position const pos = iter.current();
    TransactionRef ref;
    ref.tx_hash = pos.tx;
    ref.height = pos.block;
    page.refs.push_back(ref);
    last_emitted = pos;
    any_emitted = true;
    iter.advance(order);
  }

  // More rows remain past the page — emit a continuation cursor.
  if (any_emitted && iter.has_current()) {
    EventCursorPayload payload;
    payload.order = order;
    payload.block_height = last_emitted.block;
    payload.tx_hash = last_emitted.tx;
    payload.sequence_number = 0;
    payload.max_block_height = max_serve_block;
    std::string encoded = cursor::encode(payload);
    page.cursor.append_range(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size()));
  }

  return page;
}

} // namespace hogd
