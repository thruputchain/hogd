#include "hogd/indexer/db.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <rocksdb/version.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <rocksdb/compression_type.h>
// NOLINTNEXTLINE(misc-header-include-cycle)
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>
#include <vector>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/config.h"
#include "hogd/indexer/key_format.h"
#include "hogd/indexer/status.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

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
    contract_cf_handle_(other.contract_cf_handle_)
{
  other.tx_cf_handle_ = nullptr;
  other.contract_cf_handle_ = nullptr;
}

IndexDB &
IndexDB::operator=(IndexDB &&other) noexcept
{
  if (this != &other) {
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

  rocksdb::WriteBatch batch;
  std::string key_buffer; // Reusable buffer for key construction

  for (const auto &event_log : events) {
    // Serialize the TxEventLog for storage
    auto serialized = x23::serialize(event_log);
    std::string value(reinterpret_cast<const char *>(serialized.data()),
                      serialized.size());

    // Index by transaction (in tx_index column family)
    IndexerKeyFormat::make_tx_key(key_buffer, block_height,
                                  event_log.proposed_tx_hash);
    batch.Put(tx_cf_handle_, key_buffer, value);

    // Index by contract address (in contract_index column family)
    uint32_t sequence_number = 0;
    for (const auto &emitted_event : event_log.events) {
      IndexerKeyFormat::make_contract_key(key_buffer,
                                          emitted_event.contract_address,
                                          block_height,
                                          event_log.proposed_tx_hash,
                                          sequence_number);
      // Serialize the individual event
      auto event_serialized = x23::serialize(emitted_event);
      std::string event_value(reinterpret_cast<const char *>(
                                  event_serialized.data()),
                              event_serialized.size());
      batch.Put(contract_cf_handle_, key_buffer, event_value);
      sequence_number++;
    }
  }

  rocksdb::WriteOptions write_options;
  write_options.sync = true;
  rocksdb::Status status = db_->Write(write_options, &batch);

  if (!status.ok()) {
    error("Failed to write events to indexer: {}", status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return {};
}

IndexerStatus<TxEventLog>
IndexDB::get_tx_events(BlockHeight block_height,
                       SHA256Hash const &tx_hash) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  std::string key_buffer;
  IndexerKeyFormat::make_tx_key(key_buffer, block_height, tx_hash);
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

  TxEventLog event_log;
  try {
    x23::deserialize(&event_log, x23::BufView{reinterpret_cast<const uint8_t *>(
                                                  value.data()),
                                              value.size()});
  } catch (const x23::XdrBadInput &e) {
    error("Failed to deserialize TxEventLog: {}", e.what());
    return IndexerError(IndexerErrorCode::kInternal);
  }

  return event_log;
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

    // Deserialize the event
    std::string value_str(it->value().data(), it->value().size());
    TxEmittedEvent emitted_event;
    try {
      x23::deserialize(&emitted_event,
                       x23::BufView{reinterpret_cast<const uint8_t *>(
                                        value_str.data()),
                                    value_str.size()});
    } catch (const x23::XdrBadInput &e) {
      error("Failed to deserialize TxEmittedEvent: {}", e.what());
      continue;
    }

    // Create IdentifiedEvent
    IdentifiedEvent identified_event;
    identified_event.proposed_tx_hash = tx_hash;
    identified_event.sequence_number = sequence_number;
    identified_event.emitted_event = emitted_event;

    result.push_back(identified_event);
  }

  return result;
}

} // namespace hogd
