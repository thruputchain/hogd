#include "hogd/hog_indexer/db.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <rocksdb/db.h> // NOLINT(misc-header-include-cycle)
#include <rocksdb/iterator.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "block_query_internal.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_indexer/key_format.h"
#include "hogd/hog_indexer/status.h"
#include "indexer.hh"
#include "pagination.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
namespace {

// A method-specific tag and version, not the event/address cursor version.
constexpr uint32_t kBlockCursorFormat = 0x424c4b01;
constexpr size_t kMaxBlockCursorBytes = 128;

x23::BufView
bytes(std::string_view value)
{
  return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};
}

std::optional<uint64_t>
read_ordinal(std::string_view value)
{
  if (value.size() != sizeof(uint64_t)) {
    return std::nullopt;
  }
  uint64_t encoded;
  std::memcpy(&encoded, value.data(), sizeof(encoded));
  return std::byteswap(encoded);
}

} // namespace

std::string
IndexDB::block_query_format()
{
  // Bump when the stored layout or ConsensusValue encoding becomes
  // incompatible. Wire protocol identity alone is not a storage version.
  return "block-query-v1";
}

IndexerStatus<BlockHeight>
IndexDB::block_query_tip() const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }
  const BlockHeight tip = serve_watermark();
  if (tip < kFirstOutboxBlockHeight) {
    return IndexerError(IndexerErrorCode::kQueryNotReady);
  }
  if (!block_queries_ready_.load(std::memory_order_acquire)) {
    // Existing indexes without a complete projection remain unavailable.
    return IndexerError(IndexerErrorCode::kQueryNotReady);
  }
  return tip;
}

IndexerStatus<IndexDB::VerifiedBlockQueryRecord>
IndexDB::read_block_query_record(BlockHeight height) const
{
  std::string stored;
  const auto status =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
               IndexerKeyFormat::make_block_query_key(
                   IndexerKeyFormat::kBlockQueryRecordPrefix, height),
               &stored);
  // Called only within established coverage. A missing row is corruption.
  if (!status.ok()) {
    error("block query metadata at {} is unavailable: {}", height,
          status.ToString());
    return IndexerError(IndexerErrorCode::kInternal);
  }
  std::string identity_bytes;
  const auto identity_status =
      db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(),
               IndexerKeyFormat::make_block_identity_meta_key(height),
               &identity_bytes);
  if (!identity_status.ok()) {
    return IndexerError(IndexerErrorCode::kInternal);
  }
  try {
    BlockQueryRecord record;
    IndexerCommittedBlockIdentity identity;
    x23::deserialize(&record, bytes(stored));
    x23::deserialize(&identity, bytes(identity_bytes));
    const auto hash = sha256(record.value);
    if (record.value.proposal_header.block_height != height ||
        hash != identity.consensus_hash) {
      error("block query metadata at {} disagrees with its committed identity",
            height);
      return IndexerError(IndexerErrorCode::kInternal);
    }
    return VerifiedBlockQueryRecord{std::move(record), hash};
  } catch (const std::exception &e) {
    error("cannot decode block query metadata at {}: {}", height, e.what());
    return IndexerError(IndexerErrorCode::kInternal);
  }
}

IndexerStatus<std::optional<ConsensusValue>>
IndexDB::get_block_by_height(BlockHeight height) const
{
  auto tip = block_query_tip();
  if (!tip) {
    return tip.error();
  }
  if (height < kFirstOutboxBlockHeight || height > *tip) {
    return std::nullopt;
  }
  auto record = read_block_query_record(height);
  if (!record) {
    return record.error();
  }
  return std::move(record->record.value);
}

IndexerStatus<TransactionRefPage>
IndexDB::get_transactions_by_block(const BlockTransactionQuery &query) const
{
  auto tip = block_query_tip();
  if (!tip) {
    return tip.error();
  }
  TransactionRefPage page;
  page.latest_block = *tip;
  page.oldest_block = kFirstOutboxBlockHeight;
  IterationOrder order = query.order;
  std::optional<BlockQueryCursor> cursor;
  if (!query.cursor.empty()) {
    if (query.cursor.size() > kMaxBlockCursorBytes) {
      return page;
    }
    try {
      cursor.emplace();
      x23::deserialize(&*cursor,
                       x23::BufView(query.cursor.data(), query.cursor.size()));
    } catch (const x23::XdrException &) {
      return page;
    }
    if (cursor->format != kBlockCursorFormat ||
        cursor->height != query.height) {
      return page;
    }
    order = cursor->order;
  }
  if (!is_valid_iteration_order(order)) {
    return page;
  }
  if (cursor && query.height > page.latest_block) {
    return IndexerError(IndexerErrorCode::kQueryNotReady);
  }
  if (query.height < page.oldest_block || query.height > page.latest_block) {
    return page;
  }
  auto record = read_block_query_record(query.height);
  if (!record) {
    return record.error();
  }
  const auto block_hash = record->hash;
  if (cursor && cursor->block_hash != block_hash) {
    return page;
  }
  const bool ascending = order == IterationOrder::IterationOrderAscending;
  const std::string prefix = IndexerKeyFormat::make_block_query_key(
      IndexerKeyFormat::kBlockQueryRefPrefix, query.height);
  // The suffix after the 8-byte height bounds this exact prefix, even at
  // INT64_MAX; no height + 1 arithmetic or whole-block scan is needed.
  std::string end = prefix;
  for (size_t i = end.size(); i > 0; --i) {
    auto byte = static_cast<uint8_t>(end[i - 1]);
    end[i - 1] = static_cast<char>(byte + 1);
    if (byte != 255) {
      end.resize(i);
      break;
    }
  }
  const rocksdb::Slice lower(prefix);
  const rocksdb::Slice upper(end);
  rocksdb::ReadOptions options;
  options.iterate_lower_bound = &lower;
  options.iterate_upper_bound = &upper;
  // Indexed blocks are immutable and never pruned today. The acquired tip
  // precedes this iterator and only exposes completed atomic batches.
  std::unique_ptr<rocksdb::Iterator> iter(
      db_->NewIterator(options, db_->DefaultColumnFamily()));
  const auto transaction_count = record->record.transaction_count;
  uint64_t remaining = transaction_count;
  if (cursor) {
    const std::string key =
        IndexerKeyFormat::make_block_ref_key(query.height, cursor->last_hash);
    iter->Seek(key);
    if (!iter->status().ok()) {
      return IndexerError(IndexerErrorCode::kInternal);
    }
    if (!iter->Valid() || iter->key().ToStringView() != key) {
      return page;
    }
    auto ordinal = read_ordinal(iter->value().ToStringView());
    if (!ordinal || *ordinal >= transaction_count) {
      return IndexerError(IndexerErrorCode::kInternal);
    }
    remaining = ascending ? transaction_count - *ordinal - 1 : *ordinal;
    if (ascending) {
      iter->Next();
    }
    else {
      iter->Prev();
    }
  }
  else if (ascending) {
    iter->Seek(prefix);
  }
  else {
    iter->SeekToLast();
  }

  const uint32_t limit = clamp_events_limit(query.limit);
  while (iter->Valid()) {
    const auto key = iter->key().ToStringView();
    const auto ordinal = read_ordinal(iter->value().ToStringView());
    if (!key.starts_with(prefix) || key.size() != prefix.size() + 32 ||
        !remaining || !ordinal ||
        *ordinal !=
            (ascending ? transaction_count - remaining : remaining - 1)) {
      return IndexerError(IndexerErrorCode::kInternal);
    }
    // Validate the lookahead too before returning a continuation cursor.
    if (page.refs.size() == limit) {
      break;
    }
    TransactionRef ref;
    ref.height = query.height;
    std::memcpy(ref.tx_hash.data(), key.data() + prefix.size(),
                ref.tx_hash.size());
    page.refs.push_back(ref);
    --remaining;
    if (ascending) {
      iter->Next();
    }
    else {
      iter->Prev();
    }
  }
  if (!iter->status().ok() || (!iter->Valid() && remaining != 0)) {
    return IndexerError(IndexerErrorCode::kInternal);
  }
  if (remaining) {
    BlockQueryCursor next{.format = kBlockCursorFormat,
                          .height = query.height,
                          .block_hash = block_hash,
                          .order = order,
                          .last_hash = page.refs.back().tx_hash};
    const auto encoded = x23::serialize(next);
    page.cursor.append_range(
        std::span<const uint8_t>(encoded.data(), encoded.size()));
  }
  return page;
}

} // namespace hogd
