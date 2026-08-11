#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "event_outbox_internal.hh"
#include "hogd/base/cleanup.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "proposal.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

inline constexpr uint32_t kIndexerOutboxEnvelopeFormatVersion = 1;
inline constexpr size_t kMaxIndexerOutboxProtocolCommitBytes = 64;
inline constexpr size_t kIndexerRpcRecordMarkerBytes = sizeof(uint32_t);
inline constexpr size_t kMaxIndexerEventPullRpcRecordBytes =
    kIndexerRpcMaxMessageSize + kIndexerRpcRecordMarkerBytes;
// A durable payload can never be larger than the RPC body that must replay it.
// The actual committed limit is lower because the call header, origin, block
// identity, and record marker are also counted exactly before publication.
inline constexpr size_t kMaxIndexerOutboxPayloadBytes =
    kIndexerRpcMaxMessageSize;
// The bounded XDR header precedes raw payload bytes. Keep slack explicit so
// header reads remain fixed-size even if another fixed identity is added.
inline constexpr size_t kMaxIndexerOutboxHeaderBytes = 512;
inline constexpr size_t kMaxIndexerOutboxWrapperOverhead =
    kMaxIndexerOutboxHeaderBytes;
static_assert(kMaxIndexerOutboxPayloadBytes <=
              SIZE_MAX - kMaxIndexerOutboxWrapperOverhead);
inline constexpr size_t kMaxIndexerOutboxFileBytes =
    kMaxIndexerOutboxPayloadBytes + kMaxIndexerOutboxWrapperOverhead;

template<typename T> using IndexerOutboxStatus = std::expected<T, std::string>;

struct IndexerOutboxCommittedRecordInfo {
  BlockHeight block_height;
  std::string protocol;
  uint64_t payload_byte_length;
  SHA256Hash payload_digest;
  ConsensusValueHash consensus_hash;
  ConsensusValueHash prev_hash;

  bool operator==(const IndexerOutboxCommittedRecordInfo &) const = default;
};

struct IndexerOutboxCandidateRecordInfo {
  BlockID block_id;
  BlockHeight block_height;
  std::string protocol;
  uint64_t payload_byte_length;
  SHA256Hash payload_digest;
  EventsHash event_root;
  ProposalTxsHash proposal_txs_hash;
};

struct IndexerOutboxEpochOriginInfo {
  std::string protocol;
  uint64_t epoch_sequence;
  BlockHeight first_height;
  ConsensusValueHash first_prev_hash;
  uint64_t payload_byte_length;
  SHA256Hash payload_digest;
};

struct IndexerOutboxCanonicalBoundary {
  BlockHeight height;
  ConsensusValueHash consensus_hash;
};

struct IndexerOutboxLoadedCommittedRecord {
  IndexerOutboxCommittedRecordInfo info;
  IndexerOutboxCommittedPayload payload;
};

struct IndexerOutboxValidatedHistory {
  std::vector<IndexerOutboxCommittedRecordInfo> records;
  std::vector<IndexerOutboxEpochOriginInfo> origins;
  std::optional<IndexerOutboxEpochOrigin> current_origin;
  std::optional<IndexerOutboxCommittedRecordInfo> latest;
  std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary;
};

enum class IndexerOutboxFaultPoint {
  kVerifyPublishedInode,
  kClosePublishedTemp,
  kUnlinkPublishedTemp,
  kFsyncPublishedDirectory,
  kFsyncParentDirectory,
};

// Process-global, one-shot syscall fault injection for durability tests.
void fail_next_indexer_outbox_operation_for_test(IndexerOutboxFaultPoint point);

// These preflight helpers do not allocate, allowing boundary tests at the bulk
// RPC limit without constructing enormous buffers.
IndexerOutboxStatus<size_t>
indexer_outbox_file_size_for_payload(size_t payload_size);
IndexerOutboxStatus<void> validate_indexer_outbox_file_size(size_t file_size);
IndexerOutboxStatus<size_t> indexer_event_pull_rpc_record_size_for_parts(
    size_t consensus_value_xdr_size, size_t proposal_txs_xdr_size,
    size_t event_logs_xdr_size, size_t tx_bodies_xdr_size);

// The outbox directory is private, operator-owned state. open() requires an
// existing 0700 directory. open_or_create_in() creates indexer-outbox beneath
// its parent and syncs the parent on first creation. Both take a nonblocking
// exclusive flock on the stable directory fd before cleanup, so one process
// owns a store at a time. All later operations remain relative to this fd.
class IndexerOutboxStore {
public:
  static IndexerOutboxStatus<IndexerOutboxStore>
  open(std::string_view directory);
  static IndexerOutboxStatus<IndexerOutboxStore>
  open_or_create_in(std::string_view parent_directory);

  // Generated XDR members confuse clang-tidy's exception propagation.
  // NOLINTBEGIN(bugprone-exception-escape)
  IndexerOutboxStore(IndexerOutboxStore &&) noexcept = default;
  IndexerOutboxStore &operator=(IndexerOutboxStore &&) noexcept = default;
  // NOLINTEND(bugprone-exception-escape)
  IndexerOutboxStore(const IndexerOutboxStore &) = delete;
  IndexerOutboxStore &operator=(const IndexerOutboxStore &) = delete;

  static std::string candidate_filename(BlockID block_id);
  static std::string committed_filename(BlockHeight block_height);
  static std::string epoch_origin_filename(std::string_view protocol);
  static std::string current_epoch_origin_filename();
  x23::Async<IndexerOutboxStatus<void>>
  publish_candidate(const IndexerOutboxCandidatePayload &payload) const;
  x23::Async<IndexerOutboxStatus<IndexerOutboxCandidatePayload>>
  load_candidate(BlockID block_id) const;
  IndexerOutboxStatus<bool> has_candidate(BlockID block_id) const;
  IndexerOutboxStatus<void> remove_candidate(BlockID block_id) const;
  IndexerOutboxStatus<std::vector<IndexerOutboxCandidateRecordInfo>>
  enumerate_candidate_records_raw() const;

  x23::Async<IndexerOutboxStatus<void>>
  publish_committed(const IndexerOutboxCommittedPayload &payload) const;
  x23::Async<IndexerOutboxStatus<IndexerOutboxCommittedPayload>>
  load_committed(BlockHeight block_height) const;
  x23::Async<IndexerOutboxStatus<IndexerOutboxLoadedCommittedRecord>>
  load_committed_record(BlockHeight block_height,
                        std::string_view expected_protocol) const;
  // Reconciliation validates the whole immutable namespace before enabling
  // this lookup. Thereafter the singleton flock and no-clobber publication let
  // the latest identity come from the append cache and older identities from
  // one bounded header read without re-hashing payload bytes.
  x23::Async<
      IndexerOutboxStatus<std::optional<IndexerOutboxCommittedRecordInfo>>>
  lookup_validated_committed_record(BlockHeight block_height) const;
  IndexerOutboxStatus<bool> has_committed(BlockHeight block_height) const;
  IndexerOutboxStatus<std::optional<IndexerOutboxCommittedRecordInfo>>
  latest_committed_record_raw() const;
  IndexerOutboxStatus<IndexerOutboxCommittedRecordInfo>
  load_committed_record_info_raw(BlockHeight block_height) const;

  // Raw filename enumeration only. It sorts recognized committed identities
  // but intentionally does not assert contiguity or canonical-chain semantics;
  // callers load at most one payload at once.
  IndexerOutboxStatus<std::vector<BlockHeight>>
  enumerate_committed_heights_raw() const;
  IndexerOutboxStatus<std::vector<IndexerOutboxCommittedRecordInfo>>
  enumerate_committed_records_raw() const;
  IndexerOutboxStatus<std::vector<IndexerOutboxEpochOriginInfo>>
  enumerate_epoch_origins_raw() const;
  // Under the publication lock, validates immutable file/digest identities,
  // epoch ordering and origins, canonical continuity, and current-protocol
  // payload semantics, then installs the matching append cache.
  x23::Async<IndexerOutboxStatus<IndexerOutboxValidatedHistory>>
  validate_and_cache_committed_history_snapshot(
      std::optional<BlockHeight> max_height = std::nullopt) const;
  IndexerOutboxStatus<std::optional<IndexerOutboxEpochOrigin>>
  load_current_epoch_origin() const;
  x23::Async<IndexerOutboxStatus<void>>
  initialize_current_epoch_origin(BlockHeight first_height,
                                  ConsensusValueHash first_prev_hash) const;
  // False if any recognized final record exists or a recognized temporary
  // record was observed and cleaned while opening this store.
  IndexerOutboxStatus<bool> recognized_namespace_empty() const;
  bool recognized_temporary_residue_on_open() const
  {
    return recognized_residue_on_open_;
  }

  // Recheck the expected append tip and absence of its successor after
  // reconciliation mutations, then update the cache under the publication
  // lock.
  x23::Async<IndexerOutboxStatus<void>> cache_validated_committed_append_tip(
      std::optional<IndexerOutboxCommittedRecordInfo> latest,
      std::optional<IndexerOutboxEpochOrigin> current_origin,
      std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary) const;
  x23::Async<size_t> committed_history_scan_count_for_test() const;

private:
  explicit IndexerOutboxStore(UniqueFd directory_fd)
    : directory_fd_(std::move(directory_fd))
  {}

  static IndexerOutboxStatus<IndexerOutboxStore>
  finish_opening(UniqueFd directory_fd);
  IndexerOutboxStatus<void> cleanup_stale_temp_files();
  x23::Async<IndexerOutboxStatus<void>>
  initialize_current_epoch_origin_unlocked(
      BlockHeight first_height, ConsensusValueHash first_prev_hash) const;
  x23::Async<IndexerOutboxStatus<void>>
  initialize_committed_append_cache_unlocked() const;
  x23::Async<IndexerOutboxStatus<IndexerOutboxValidatedHistory>>
  validate_committed_history_unlocked(
      std::optional<BlockHeight> max_height = std::nullopt) const;
  IndexerOutboxStatus<std::vector<std::string>> list_names() const;
  IndexerOutboxStatus<void>
  publish_bytes(std::string_view filename,
                std::span<const std::byte> bytes) const;
  IndexerOutboxStatus<std::vector<uint8_t>>
  load_bytes(std::string_view filename) const;
  IndexerOutboxStatus<IndexerOutboxRecordHeader>
  load_header(std::string_view filename) const;
  IndexerOutboxStatus<IndexerOutboxCommittedRecordInfo>
  load_committed_record_info_header(BlockHeight block_height) const;
  IndexerOutboxStatus<std::vector<uint8_t>>
  load_payload(std::string_view filename,
               const IndexerOutboxRecordHeader &header) const;
  IndexerOutboxStatus<void>
  validate_payload_digest(std::string_view filename,
                          const IndexerOutboxRecordHeader &header) const;
  IndexerOutboxStatus<bool> has_regular_file(std::string_view filename) const;

  UniqueFd directory_fd_;
  bool recognized_residue_on_open_ = false;
  // The generated XDR members are movable without allocation, but clang-tidy
  // follows an infeasible copying path through their implicit move functions.
  struct CommittedAppendCache { // NOLINT(bugprone-exception-escape)
    std::optional<IndexerOutboxCommittedRecordInfo> latest;
    std::optional<IndexerOutboxEpochOrigin> current_origin;
    std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary;
  };
  // There is intentionally no durable latest sidecar: publication of a record
  // and sidecar could not be atomic. Reconciliation/open validation seeds this
  // cache, or direct-store callers rebuild it once lazily. The directory flock
  // guarantees this process is the singleton writer; the coroutine lock
  // serializes its threads without blocking one worker on another. The cache
  // advances only after durable no-clobber publication.
  mutable std::unique_ptr<x23::CoroLock> committed_publication_lock_ =
      std::make_unique<x23::CoroLock>();
  mutable std::optional<CommittedAppendCache> committed_append_cache_;
  mutable size_t committed_history_scan_count_ = 0;
};

} // namespace hogd
