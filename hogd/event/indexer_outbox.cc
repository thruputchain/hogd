#include "hogd/event/indexer_outbox.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <expected>
#include <fcntl.h>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdio.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "event_outbox_internal.hh"
#include "hogd/base/cleanup.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
namespace {

constexpr std::string_view kCandidatePrefix = "candidate-";
constexpr std::string_view kCommittedPrefix = "committed-";
constexpr std::string_view kEpochOriginPrefix = "epoch-origin-";
constexpr std::string_view kOutboxSuffix = ".outbox";
constexpr std::string_view kTempMarker = ".tmp.";
constexpr std::string_view kOutboxDirectory = "indexer-outbox";
constexpr std::string_view kIndexerSlotFilename = "indexer-slot.outbox";
constexpr size_t kIdentityDigits = 20;
static_assert(x23::detail::xdr_size_bounds<ConsensusValue>().max_ <
              kIndexerRpcMaxMessageSize);

std::atomic<int> injected_fault{-1};

bool
consume_fault(IndexerOutboxFaultPoint point)
{
  int expected = static_cast<int>(point);
  return injected_fault.compare_exchange_strong(expected, -1);
}

std::string
system_error(std::string_view operation, int error)
{
  return std::format("{}: {}", operation, std::strerror(error));
}

std::optional<size_t>
checked_add(size_t lhs, size_t rhs)
{
  if (lhs > std::numeric_limits<size_t>::max() - rhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

template<typename Integer>
std::optional<Integer>
parse_filename_identity(std::string_view filename, std::string_view prefix)
{
  const size_t expected_size =
      prefix.size() + kIdentityDigits + kOutboxSuffix.size();
  if (filename.size() != expected_size || !filename.starts_with(prefix) ||
      !filename.ends_with(kOutboxSuffix)) {
    return std::nullopt;
  }

  std::string_view digits = filename.substr(prefix.size(), kIdentityDigits);
  Integer identity;
  auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), identity);
  if (error != std::errc{} || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return identity;
}

bool
is_decimal(std::string_view value)
{
  return !value.empty() && std::ranges::all_of(value, [](char c) {
    return c >= '0' && c <= '9';
  });
}

bool
is_epoch_origin_filename(std::string_view filename)
{
  if (!filename.starts_with(kEpochOriginPrefix) ||
      !filename.ends_with(kOutboxSuffix)) {
    return false;
  }
  std::string_view identity =
      filename.substr(kEpochOriginPrefix.size(), filename.size() -
                                                     kEpochOriginPrefix.size() -
                                                     kOutboxSuffix.size());
  return !identity.empty() && std::ranges::all_of(identity, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

bool
is_recognized_final_filename(std::string_view filename)
{
  return parse_filename_identity<BlockID>(filename, kCandidatePrefix)
             .has_value() ||
         parse_filename_identity<BlockHeight>(filename, kCommittedPrefix)
             .has_value() ||
         is_epoch_origin_filename(filename);
}

bool
is_recognized_temp_filename(std::string_view filename)
{
  const size_t marker = filename.find(kTempMarker);
  if (marker == std::string_view::npos ||
      (!is_recognized_final_filename(filename.substr(0, marker)) &&
       filename.substr(0, marker) != kIndexerSlotFilename)) {
    return false;
  }
  std::string_view identity = filename.substr(marker + kTempMarker.size());
  const size_t separator = identity.find('.');
  return separator != std::string_view::npos &&
         is_decimal(identity.substr(0, separator)) &&
         is_decimal(identity.substr(separator + 1));
}

bool
same_inode(const struct stat &lhs, const struct stat &rhs)
{
  return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino;
}

bool
nondeterministic_data_equal(const NondeterministicProposalData &lhs,
                            const NondeterministicProposalData &rhs)
{
  return x23::serialize(lhs) == x23::serialize(rhs);
}

template<typename Payload>
IndexerOutboxStatus<void>
validate_payload_sizes(const Payload &payload)
{
  size_t consensus_value_size = 0;
  if constexpr (std::same_as<Payload, IndexerOutboxCommittedPayload>) {
    consensus_value_size = x23::xdr_size(payload.consensus_value);
  }
  else {
    consensus_value_size = x23::detail::xdr_size_bounds<ConsensusValue>().max_;
  }
  auto delivery_size = indexer_event_apply_rpc_record_size_for_parts(
      consensus_value_size, x23::xdr_size(payload.proposal_txs),
      x23::xdr_size(payload.event_logs), x23::xdr_size(payload.tx_bodies));
  if (!delivery_size) {
    return std::unexpected(delivery_size.error());
  }
  auto file_size = indexer_outbox_file_size_for_payload(x23::xdr_size(payload));
  if (!file_size) {
    return std::unexpected(file_size.error());
  }
  return {};
}

template<typename Payload>
x23::Async<IndexerOutboxStatus<ProposalTxsHash>>
validate_canonical_proposal_mapping(const Payload &payload)
{
  x23::XdrVec<SignedTransactionHash> canonical_hashes;
  x23::XdrVec<NondeterministicProposalData> canonical_nondeterministic_data;
  if (!(co_await parallel_batch_to_t(payload.proposal_txs.hash,
                                     canonical_hashes)) ||
      !(co_await parallel_batch_to_t(payload.proposal_txs.nondeterministic_data,
                                     canonical_nondeterministic_data))) {
    co_return std::unexpected("malformed canonical ProposalTxs parallel batch");
  }
  if (canonical_hashes.size() != canonical_nondeterministic_data.size()) {
    co_return std::unexpected(
        "canonical ProposalTxs batch cardinalities differ");
  }
  if (canonical_hashes.size() != payload.event_logs.size() ||
      canonical_hashes.size() != payload.tx_bodies.size()) {
    co_return std::unexpected(
        "canonical proposal and event/body batch cardinalities differ");
  }

  std::unordered_map<SignedTransactionHash, NondeterministicProposalData,
                     ByteArrayHasher>
      canonical;
  canonical.reserve(canonical_hashes.size());
  for (size_t i = 0; i < canonical_hashes.size(); ++i) {
    if (!canonical
             .emplace(canonical_hashes[i], canonical_nondeterministic_data[i])
             .second) {
      co_return std::unexpected(
          "canonical ProposalTxs contains duplicate hashes");
    }
  }

  std::unordered_set<SignedTransactionHash, ByteArrayHasher> seen_bodies;
  seen_bodies.reserve(payload.tx_bodies.size());
  for (size_t i = 0; i < payload.tx_bodies.size(); ++i) {
    const SignedTransactionHash body_hash =
        sha256(payload.tx_bodies[i].signed_tx.tx);
    if (body_hash != payload.event_logs[i].proposed_tx_hash) {
      co_return std::unexpected(std::format(
          "transaction body hash differs from event log at index {}", i));
    }
    if (!seen_bodies.insert(body_hash).second) {
      co_return std::unexpected("event/body batches contain duplicate hashes");
    }
    auto entry = canonical.find(body_hash);
    if (entry == canonical.end()) {
      co_return std::unexpected(
          "event/body transaction is missing from canonical ProposalTxs");
    }
    if (!nondeterministic_data_equal(
            payload.tx_bodies[i].nondeterministic_proposal_data,
            entry->second)) {
      co_return std::unexpected(
          "transaction body nondeterministic data differs from canonical "
          "ProposalTxs");
    }
  }
  if (seen_bodies.size() != canonical.size()) {
    co_return std::unexpected(
        "canonical ProposalTxs contains extra transactions");
  }
  co_return proposal_txs_compatibility_hash(payload.proposal_txs);
}

template<typename Payload>
x23::Async<IndexerOutboxStatus<ProposalTxsHash>>
validate_batches(const Payload &payload, const SHA256Hash &expected_event_root)
{
  if (auto sizes = validate_payload_sizes(payload); !sizes) {
    co_return std::unexpected(sizes.error());
  }
  if (payload.event_logs.size() != payload.tx_bodies.size()) {
    co_return std::unexpected(
        "event log and transaction body batch sizes differ");
  }
  if (compute_indexer_outbox_event_root(payload.event_logs) !=
      expected_event_root) {
    co_return std::unexpected("event root differs from payload identity");
  }
  co_return co_await validate_canonical_proposal_mapping(payload);
}

x23::Async<IndexerOutboxStatus<void>>
validate_candidate(const IndexerOutboxCandidatePayload &payload)
{
  if (payload.format_version != kIndexerOutboxFormatVersion) {
    co_return std::unexpected(
        std::format("unknown candidate outbox format version {}",
                    payload.format_version));
  }
  if (payload.block_height < 0) {
    co_return std::unexpected("candidate outbox height is negative");
  }
  auto proposal_hash = co_await validate_batches(payload, payload.event_root);
  if (!proposal_hash) {
    co_return std::unexpected(proposal_hash.error());
  }
  co_return IndexerOutboxStatus<void>{};
}

x23::Async<IndexerOutboxStatus<void>>
validate_committed(const IndexerOutboxCommittedPayload &payload,
                   BlockHeight expected_height)
{
  const BlockHeight height =
      payload.consensus_value.proposal_header.block_height;
  if (height < 0) {
    co_return std::unexpected("committed outbox height is negative");
  }
  if (height != expected_height) {
    co_return std::unexpected(
        "committed filename height differs from ConsensusValue");
  }
  if (auto sizes = validate_payload_sizes(payload); !sizes) {
    co_return sizes;
  }
  co_return co_await validate_indexer_committed_payload(payload);
}

IndexerOutboxRecordHeader
make_header(const IndexerOutboxCandidatePayload &payload, size_t payload_size,
            SHA256Hash digest)
{
  IndexerOutboxRecordHeader header{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .block_height = payload.block_height,
      .protocol = version::kHogdProtocol(),
      .payload_byte_length = payload_size,
      .payload_digest = digest,
      .event_root = payload.event_root,
      .proposal_txs_hash =
          proposal_txs_compatibility_hash(payload.proposal_txs),
  };
  header.record_identity.kind(IndexerOutboxRecordKind::IndexerOutboxCandidate);
  header.record_identity.candidate_block_id() = payload.block_id;
  return header;
}

IndexerOutboxRecordHeader
make_header(const IndexerOutboxCommittedPayload &payload, size_t payload_size,
            SHA256Hash digest)
{
  IndexerOutboxRecordHeader header{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .block_height = payload.consensus_value.proposal_header.block_height,
      .protocol = version::kHogdProtocol(),
      .payload_byte_length = payload_size,
      .payload_digest = digest,
      .consensus_hash = sha256(payload.consensus_value),
      .prev_hash = payload.consensus_value.prev_hash,
      .event_root = payload.consensus_value.proposal_header.events_hash,
      .proposal_txs_hash = payload.consensus_value.proposal_txs_hash,
  };
  header.record_identity.kind(IndexerOutboxRecordKind::IndexerOutboxCommitted);
  return header;
}

IndexerOutboxRecordHeader
make_header(const IndexerOutboxEpochOrigin &payload, size_t payload_size,
            SHA256Hash digest)
{
  IndexerOutboxRecordHeader header{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .epoch_sequence = payload.epoch_sequence,
      .block_height = payload.origin.first_height,
      .protocol = version::kHogdProtocol(),
      .payload_byte_length = payload_size,
      .payload_digest = digest,
      .prev_hash = payload.origin.first_prev_hash,
  };
  header.record_identity.kind(
      IndexerOutboxRecordKind::IndexerOutboxEpochOrigin);
  return header;
}

template<typename Payload>
IndexerOutboxStatus<x23::Buf>
encode_file(const Payload &payload)
{
  const size_t payload_size = x23::xdr_size(payload);
  if (auto size = indexer_outbox_file_size_for_payload(payload_size); !size) {
    return std::unexpected(size.error());
  }

  x23::Buf payload_bytes = x23::serialize(payload);
  IndexerOutboxRecordHeader header =
      make_header(payload, payload_bytes.size(),
                  sha256(x23::BufView(payload_bytes)));
  x23::Buf header_bytes = x23::serialize(header);
  if (header_bytes.size() > kMaxIndexerOutboxHeaderBytes) {
    return std::unexpected("indexer outbox header exceeds bounded limit");
  }
  x23::Buf file_bytes =
      x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  file_bytes.append_range(header_bytes);
  file_bytes.append_range(payload_bytes);
  if (auto valid = validate_indexer_outbox_file_size(file_bytes.size());
      !valid) {
    return std::unexpected(valid.error());
  }
  return file_bytes;
}

template<typename Payload>
IndexerOutboxStatus<Payload>
decode_payload(std::span<const uint8_t> bytes, std::string_view kind)
{
  Payload payload;
  try {
    x23::deserialize(&payload, x23::BufView(bytes));
  } catch (const x23::XdrException &e) {
    return std::unexpected(
        std::format("malformed {} outbox payload XDR: {}", kind, e.what()));
  }
  if (x23::xdr_size(payload) != bytes.size()) {
    return std::unexpected(
        std::format("{} outbox payload has trailing bytes", kind));
  }
  return payload;
}

} // namespace

void
fail_next_indexer_outbox_operation_for_test(IndexerOutboxFaultPoint point)
{
  int expected = -1;
  CHECK(injected_fault.compare_exchange_strong(expected,
                                               static_cast<int>(point)));
}

IndexerOutboxStatus<size_t>
indexer_outbox_file_size_for_payload(size_t payload_size)
{
  if (payload_size > kMaxIndexerOutboxPayloadBytes) {
    return std::unexpected("indexer outbox payload exceeds durable limit");
  }
  if (std::string_view(version::kHogdProtocol()).size() >
      kMaxIndexerOutboxProtocolCommitBytes) {
    return std::unexpected("hogd protocol token exceeds outbox envelope limit");
  }
  IndexerOutboxRecordHeader header{};
  header.protocol = version::kHogdProtocol();
  const size_t header_size = x23::xdr_size(header);
  if (header_size > kMaxIndexerOutboxHeaderBytes) {
    return std::unexpected("indexer outbox header exceeds bounded limit");
  }
  auto size = checked_add(4, header_size);
  size = size.and_then(
      [&](size_t value) { return checked_add(value, payload_size); });
  if (!size || *size > kMaxIndexerOutboxFileBytes) {
    return std::unexpected("indexer outbox file exceeds size limit");
  }
  return *size;
}

IndexerOutboxStatus<void>
validate_indexer_outbox_file_size(size_t file_size)
{
  if (file_size > kMaxIndexerOutboxFileBytes) {
    return std::unexpected("indexer outbox file exceeds size limit");
  }
  return {};
}

IndexerOutboxStatus<size_t>
indexer_event_apply_rpc_record_size_for_parts(size_t consensus_value_xdr_size,
                                              size_t proposal_txs_xdr_size,
                                              size_t event_logs_xdr_size,
                                              size_t tx_bodies_xdr_size)
{
  const auto call = x23::call_msg<IndexerEvent1::proc_meta<12>>();
  IndexerOutboxOrigin origin{.protocol = version::kHogdProtocol()};
  // Record marker + RPC call header + apply arguments. The committed block is
  // its payload fields followed by the two 32-byte identity hashes.
  auto size = checked_add(kIndexerRpcRecordMarkerBytes, x23::xdr_size(call));
  size = size.and_then(
      [&](size_t value) { return checked_add(value, x23::xdr_size(origin)); });
  size = size.and_then([&](size_t value) { return checked_add(value, 4); });
  size = size.and_then([&](size_t value) {
    return checked_add(value, consensus_value_xdr_size);
  });
  size = size.and_then(
      [&](size_t value) { return checked_add(value, proposal_txs_xdr_size); });
  size = size.and_then(
      [&](size_t value) { return checked_add(value, event_logs_xdr_size); });
  size = size.and_then(
      [&](size_t value) { return checked_add(value, tx_bodies_xdr_size); });
  size = size.and_then([&](size_t value) {
    return checked_add(value, x23::xdr_size(IndexerCommittedBlockIdentity{}));
  });
  if (!size || *size > kMaxIndexerEventApplyRpcRecordBytes) {
    return std::unexpected(
        "IndexerEvent1 apply RPC record exceeds x23 31-bit limit");
  }
  return *size;
}

IndexerOutboxStatus<IndexerOutboxStore>
IndexerOutboxStore::open(std::string_view directory)
{
  const std::string path(directory);
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return std::unexpected(
        system_error("open existing indexer outbox directory", errno));
  }
  UniqueFd directory_fd(fd);
  return finish_opening(std::move(directory_fd));
}

IndexerOutboxStatus<IndexerOutboxStore>
IndexerOutboxStore::open_or_create_in(std::string_view parent_directory)
{
  const std::string parent_path(parent_directory);
  const int parent_fd = ::open(parent_path.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parent_fd < 0) {
    return std::unexpected(
        system_error("open indexer outbox parent directory", errno));
  }
  UniqueFd parent(parent_fd);

  const std::string directory_name(kOutboxDirectory);
  if (mkdirat(parent.get(), directory_name.c_str(), 0700) != 0 &&
      errno != EEXIST) {
    return std::unexpected(
        system_error("create indexer outbox directory", errno));
  }
  if (consume_fault(IndexerOutboxFaultPoint::kFsyncParentDirectory)) {
    return std::unexpected("injected fsync indexer outbox parent directory");
  }
  if (fsync(parent.get()) != 0) {
    return std::unexpected(
        system_error("fsync indexer outbox parent directory", errno));
  }

  const int directory_fd =
      openat(parent.get(), directory_name.c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_fd < 0) {
    return std::unexpected(
        system_error("open indexer outbox directory", errno));
  }
  return finish_opening(UniqueFd(directory_fd));
}

IndexerOutboxStatus<IndexerOutboxStore>
IndexerOutboxStore::finish_opening(UniqueFd directory_fd)
{
  struct stat stat_buf{};
  if (fstat(directory_fd.get(), &stat_buf) != 0) {
    return std::unexpected(
        system_error("stat indexer outbox directory", errno));
  }
  if (!S_ISDIR(stat_buf.st_mode)) {
    return std::unexpected("indexer outbox path is not a directory");
  }
  if ((stat_buf.st_mode & 07777) != 0700) {
    return std::unexpected("indexer outbox directory permissions are not 0700");
  }
  if (flock(directory_fd.get(), LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    if (error == EWOULDBLOCK || error == EAGAIN) {
      return std::unexpected("indexer outbox directory is already locked");
    }
    return std::unexpected(
        system_error("lock indexer outbox directory", error));
  }

  IndexerOutboxStore store(std::move(directory_fd));
  if (auto cleaned = store.cleanup_stale_temp_files(); !cleaned) {
    return std::unexpected(cleaned.error());
  }
  return store;
}

std::string
IndexerOutboxStore::candidate_filename(BlockID block_id)
{
  return std::format("{}{:020}{}", kCandidatePrefix, block_id, kOutboxSuffix);
}

std::string
IndexerOutboxStore::committed_filename(BlockHeight block_height)
{
  return std::format("{}{:020}{}", kCommittedPrefix, block_height,
                     kOutboxSuffix);
}

std::string
IndexerOutboxStore::epoch_origin_filename(std::string_view protocol)
{
  std::string identity(protocol);
  for (char &c : identity) {
    const bool alphanumeric = (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alphanumeric) {
      c = '_';
    }
  }
  return std::format("{}{}{}", kEpochOriginPrefix, identity, kOutboxSuffix);
}

std::string
IndexerOutboxStore::current_epoch_origin_filename()
{
  return epoch_origin_filename(version::kHogdProtocol());
}

std::string
IndexerOutboxStore::indexer_slot_filename()
{
  return std::string(kIndexerSlotFilename);
}

IndexerOutboxStatus<std::optional<EventIndexerSlot>>
IndexerOutboxStore::load_indexer_slot() const
{
  auto exists = has_regular_file(kIndexerSlotFilename);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return std::optional<EventIndexerSlot>{};
  }
  auto bytes = load_bytes(kIndexerSlotFilename);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  EventIndexerSlot slot;
  try {
    x23::deserialize(&slot, x23::BufView(*bytes));
  } catch (const x23::XdrException &e) {
    return std::unexpected(
        std::format("malformed durable indexer slot: {}", e.what()));
  }
  if (slot.format_version != kEventIndexerSlotFormatVersion ||
      x23::xdr_size(slot) != bytes->size()) {
    return std::unexpected("invalid durable indexer slot metadata");
  }
  return std::optional(std::move(slot));
}

IndexerOutboxStatus<void>
IndexerOutboxStore::store_indexer_slot(const EventIndexerSlot &slot) const
{
  if (slot.format_version != kEventIndexerSlotFormatVersion) {
    return std::unexpected("invalid durable indexer slot version");
  }
  const x23::Buf bytes = x23::serialize(slot);
  std::lock_guard lock(*slot_publication_mutex_);
  return replace_bytes(kIndexerSlotFilename, std::as_bytes(std::span(bytes)));
}

x23::Async<IndexerOutboxStatus<void>>
IndexerOutboxStore::publish_candidate(
    const IndexerOutboxCandidatePayload &payload) const
{
  if (auto valid = co_await validate_candidate(payload); !valid) {
    co_return valid;
  }
  auto bytes = encode_file(payload);
  if (!bytes) {
    co_return std::unexpected(bytes.error());
  }
  co_return publish_bytes(candidate_filename(payload.block_id),
                          std::as_bytes(std::span(*bytes)));
}

x23::Async<IndexerOutboxStatus<IndexerOutboxCandidatePayload>>
IndexerOutboxStore::load_candidate(BlockID block_id) const
{
  const std::string filename = candidate_filename(block_id);
  auto header = load_header(filename);
  if (!header) {
    co_return std::unexpected(header.error());
  }
  if (header->record_identity.kind() !=
          IndexerOutboxRecordKind::IndexerOutboxCandidate ||
      header->record_identity.candidate_block_id() != block_id) {
    co_return std::unexpected("candidate outbox header identity mismatch");
  }
  if (std::string_view(header->protocol) != version::kHogdProtocol()) {
    co_return std::unexpected("outbox protocol does not match this binary");
  }
  auto bytes = load_payload(filename, *header);
  if (!bytes) {
    co_return std::unexpected(bytes.error());
  }
  auto payload =
      decode_payload<IndexerOutboxCandidatePayload>(*bytes, "candidate");
  if (!payload) {
    co_return payload;
  }
  if (payload->block_id != header->record_identity.candidate_block_id() ||
      payload->block_height != header->block_height ||
      payload->event_root != header->event_root ||
      proposal_txs_compatibility_hash(payload->proposal_txs) !=
          header->proposal_txs_hash) {
    co_return std::unexpected(
        "candidate outbox header/payload metadata mismatch");
  }
  if (auto valid = co_await validate_candidate(*payload); !valid) {
    co_return std::unexpected(valid.error());
  }
  co_return payload;
}

IndexerOutboxStatus<bool>
IndexerOutboxStore::has_candidate(BlockID block_id) const
{
  return has_regular_file(candidate_filename(block_id));
}

IndexerOutboxStatus<std::vector<BlockID>>
IndexerOutboxStore::enumerate_candidate_block_ids_raw() const
{
  auto records = enumerate_candidate_records_raw();
  if (!records) {
    return std::unexpected(records.error());
  }
  std::vector<BlockID> block_ids;
  block_ids.reserve(records->size());
  for (const auto &record : *records) {
    block_ids.push_back(record.block_id);
  }
  return block_ids;
}

IndexerOutboxStatus<std::vector<IndexerOutboxCandidateRecordInfo>>
IndexerOutboxStore::enumerate_candidate_records_raw() const
{
  auto names = list_names();
  if (!names) {
    return std::unexpected(names.error());
  }
  std::vector<IndexerOutboxCandidateRecordInfo> records;
  for (const auto &name : *names) {
    auto block_id = parse_filename_identity<BlockID>(name, kCandidatePrefix);
    if (!block_id) {
      continue;
    }
    auto header = load_header(name);
    if (!header) {
      return std::unexpected(header.error());
    }
    if (header->record_identity.kind() !=
            IndexerOutboxRecordKind::IndexerOutboxCandidate ||
        header->record_identity.candidate_block_id() != *block_id) {
      return std::unexpected("candidate outbox filename/header mismatch");
    }
    if (auto valid = validate_payload_digest(name, *header); !valid) {
      return std::unexpected(valid.error());
    }
    records.push_back({.block_id = header->record_identity.candidate_block_id(),
                       .block_height = header->block_height,
                       .protocol = std::string(header->protocol),
                       .payload_byte_length = header->payload_byte_length,
                       .payload_digest = header->payload_digest,
                       .event_root = header->event_root,
                       .proposal_txs_hash = header->proposal_txs_hash});
  }
  std::ranges::sort(records, [](const auto &lhs, const auto &rhs) {
    return lhs.block_id < rhs.block_id;
  });
  return records;
}

IndexerOutboxStatus<void>
IndexerOutboxStore::remove_candidate(BlockID block_id) const
{
  const std::string filename = candidate_filename(block_id);
  if (unlinkat(directory_fd_.get(), filename.c_str(), 0) != 0) {
    if (errno == ENOENT) {
      return {};
    }
    return std::unexpected(
        system_error("remove candidate indexer outbox file", errno));
  }
  if (fsync(directory_fd_.get()) != 0) {
    return std::unexpected(
        system_error("fsync indexer outbox directory after candidate removal",
                     errno));
  }
  return {};
}

x23::Async<IndexerOutboxStatus<void>>
IndexerOutboxStore::publish_committed(
    const IndexerOutboxCommittedPayload &payload) const
{
  const BlockHeight height =
      payload.consensus_value.proposal_header.block_height;
  if (auto valid = co_await validate_committed(payload, height); !valid) {
    co_return valid;
  }
  auto bytes = encode_file(payload);
  if (!bytes) {
    co_return std::unexpected(bytes.error());
  }

  x23::CoroGuard publication_guard;
  co_await publication_guard(*committed_publication_lock_);
  auto exists = has_committed(height);
  if (!exists) {
    co_return std::unexpected(exists.error());
  }
  if (*exists) {
    // publish_bytes accepts only a byte-identical existing file. This check
    // intentionally precedes latest-height validation so an exact retry of an
    // older committed height remains idempotent.
    auto published = publish_bytes(committed_filename(height),
                                   std::as_bytes(std::span(*bytes)));
    if (!published) {
      committed_append_cache_.reset();
      co_return published;
    }
    // A prior attempt may have linked this exact record and then failed a
    // close/unlink/directory-fsync step. Rebuild once so this retry repairs the
    // append tip; an older duplicate is observed but cannot regress it.
    if (auto initialized =
            co_await initialize_committed_append_cache_unlocked();
        !initialized) {
      committed_append_cache_.reset();
      co_return initialized;
    }
    co_return IndexerOutboxStatus<void>{};
  }

  if (auto initialized = co_await initialize_committed_append_cache_unlocked();
      !initialized) {
    co_return initialized;
  }
  if (!committed_append_cache_) {
    co_return std::unexpected("committed append cache was not initialized");
  }
  CommittedAppendCache &cache = committed_append_cache_.value();
  const std::string_view current_protocol = version::kHogdProtocol();
  const bool current_epoch_started =
      cache.latest && cache.latest->protocol == current_protocol;
  if (!current_epoch_started) {
    if (cache.current_origin) {
      if (height != cache.current_origin->origin.first_height ||
          payload.consensus_value.prev_hash !=
              cache.current_origin->origin.first_prev_hash) {
        co_return std::unexpected(
            "first committed record does not match current outbox epoch "
            "origin");
      }
      if (cache.latest && (cache.latest->block_height == INT64_MAX ||
                           cache.current_origin->origin.first_height !=
                               cache.latest->block_height + 1 ||
                           cache.current_origin->origin.first_prev_hash !=
                               cache.latest->consensus_hash)) {
        co_return std::unexpected(
            "current outbox epoch origin does not continue archival records");
      }
      if (!cache.latest && cache.canonical_boundary &&
          (cache.current_origin->origin.first_height !=
               cache.canonical_boundary->height ||
           cache.current_origin->origin.first_prev_hash !=
               cache.canonical_boundary->consensus_hash)) {
        co_return std::unexpected(
            "current outbox epoch origin does not continue canonical boundary");
      }
    }
    else {
      co_return std::unexpected(
          "current outbox epoch origin must be established before committed "
          "publication");
    }
  }
  else {
    if (!cache.current_origin) {
      co_return std::unexpected(
          "committed current outbox records have no epoch origin");
    }
    if (cache.latest->block_height == INT64_MAX ||
        height != cache.latest->block_height + 1) {
      co_return std::unexpected(
          "new committed outbox height is not the next canonical height");
    }
    if (payload.consensus_value.prev_hash != cache.latest->consensus_hash) {
      co_return std::unexpected(
          "new committed outbox record does not link to previous consensus "
          "value");
    }
  }

  auto published = publish_bytes(committed_filename(height),
                                 std::as_bytes(std::span(*bytes)));
  if (!published) {
    committed_append_cache_.reset();
    co_return published;
  }
  auto header = load_header(committed_filename(height));
  if (!header) {
    committed_append_cache_.reset();
    co_return std::unexpected(std::format(
        "committed record published but append cache update failed: {}",
        header.error()));
  }
  cache.latest = IndexerOutboxCommittedRecordInfo{
      .block_height = header->block_height,
      .protocol = std::string(header->protocol),
      .payload_byte_length = header->payload_byte_length,
      .payload_digest = header->payload_digest,
      .consensus_hash = header->consensus_hash,
      .prev_hash = header->prev_hash,
  };
  if (header->block_height == INT64_MAX) {
    cache.canonical_boundary.reset();
  }
  else {
    cache.canonical_boundary = IndexerOutboxCanonicalBoundary{
        .height = header->block_height + 1,
        .consensus_hash = header->consensus_hash,
    };
  }
  co_return IndexerOutboxStatus<void>{};
}

x23::Async<IndexerOutboxStatus<IndexerOutboxCommittedPayload>>
IndexerOutboxStore::load_committed(BlockHeight block_height) const
{
  if (block_height < 0) {
    co_return std::unexpected("committed outbox height is negative");
  }
  const std::string filename = committed_filename(block_height);
  auto header = load_header(committed_filename(block_height));
  if (!header) {
    co_return std::unexpected(header.error());
  }
  if (std::string_view(header->protocol) != version::kHogdProtocol()) {
    co_return std::unexpected("outbox protocol does not match this binary");
  }
  if (header->record_identity.kind() !=
          IndexerOutboxRecordKind::IndexerOutboxCommitted ||
      header->block_height != block_height) {
    co_return std::unexpected("committed outbox header identity mismatch");
  }
  auto bytes = load_payload(filename, *header);
  if (!bytes) {
    co_return std::unexpected(bytes.error());
  }
  auto payload =
      decode_payload<IndexerOutboxCommittedPayload>(*bytes, "committed");
  if (!payload) {
    co_return payload;
  }
  if (auto valid = co_await validate_committed(*payload, block_height);
      !valid) {
    co_return std::unexpected(valid.error());
  }
  if (sha256(payload->consensus_value) != header->consensus_hash ||
      payload->consensus_value.prev_hash != header->prev_hash ||
      payload->consensus_value.proposal_header.events_hash !=
          header->event_root ||
      payload->consensus_value.proposal_txs_hash != header->proposal_txs_hash) {
    co_return std::unexpected(
        "committed outbox header/payload metadata mismatch");
  }
  co_return payload;
}

IndexerOutboxStatus<std::optional<IndexerOutboxEpochOrigin>>
IndexerOutboxStore::load_current_epoch_origin() const
{
  auto exists = has_regular_file(current_epoch_origin_filename());
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return std::optional<IndexerOutboxEpochOrigin>{};
  }
  const std::string filename = current_epoch_origin_filename();
  auto header = load_header(filename);
  if (!header) {
    return std::unexpected(header.error());
  }
  if (header->record_identity.kind() !=
          IndexerOutboxRecordKind::IndexerOutboxEpochOrigin ||
      std::string_view(header->protocol) != version::kHogdProtocol()) {
    return std::unexpected("invalid current indexer outbox epoch header");
  }
  auto bytes = load_payload(filename, *header);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  auto origin =
      decode_payload<IndexerOutboxEpochOrigin>(*bytes, "epoch origin");
  if (!origin) {
    return std::unexpected(origin.error());
  }
  if (origin->format_version != kIndexerOutboxEnvelopeFormatVersion ||
      origin->epoch_sequence != header->epoch_sequence ||
      origin->origin.epoch_sequence != header->epoch_sequence ||
      std::string_view(origin->origin.protocol) != version::kHogdProtocol() ||
      origin->origin.first_height < 0) {
    return std::unexpected("invalid current indexer outbox epoch origin");
  }
  if (origin->origin.first_height != header->block_height ||
      origin->origin.first_prev_hash != header->prev_hash) {
    return std::unexpected("epoch origin header/payload metadata mismatch");
  }
  return std::optional<IndexerOutboxEpochOrigin>(std::move(*origin));
}

x23::Async<IndexerOutboxStatus<void>>
IndexerOutboxStore::initialize_current_epoch_origin(
    BlockHeight first_height, ConsensusValueHash first_prev_hash) const
{
  x23::CoroGuard publication_guard;
  co_await publication_guard(*committed_publication_lock_);
  co_return co_await initialize_current_epoch_origin_unlocked(
      first_height, std::move(first_prev_hash));
}

x23::Async<IndexerOutboxStatus<void>>
IndexerOutboxStore::initialize_current_epoch_origin_unlocked(
    BlockHeight first_height, ConsensusValueHash first_prev_hash) const
{
  if (first_height < 0) {
    co_return std::unexpected("indexer outbox epoch origin height is negative");
  }
  auto existing = load_current_epoch_origin();
  if (!existing) {
    co_return std::unexpected(existing.error());
  }
  if (*existing) {
    if ((*existing)->origin.first_height != first_height ||
        (*existing)->origin.first_prev_hash != first_prev_hash) {
      co_return std::unexpected(
          "conflicting current indexer outbox epoch origin");
    }
    if (consume_fault(IndexerOutboxFaultPoint::kFsyncPublishedDirectory)) {
      co_return std::unexpected("injected fsync indexer outbox directory");
    }
    if (fsync(directory_fd_.get()) != 0) {
      co_return std::unexpected(
          system_error("fsync indexer outbox directory", errno));
    }
    co_return IndexerOutboxStatus<void>{};
  }
  if (auto initialized = co_await initialize_committed_append_cache_unlocked();
      !initialized) {
    co_return initialized;
  }
  auto origins = enumerate_epoch_origins_raw();
  if (!origins) {
    co_return std::unexpected(origins.error());
  }
  if (!committed_append_cache_) {
    co_return std::unexpected("committed append cache was not initialized");
  }
  const CommittedAppendCache &cache = committed_append_cache_.value();
  if (cache.canonical_boundary &&
      (first_height != cache.canonical_boundary->height ||
       first_prev_hash != cache.canonical_boundary->consensus_hash)) {
    co_return std::unexpected(
        "current indexer outbox epoch origin does not match canonical "
        "boundary");
  }
  if (!cache.canonical_boundary && (cache.latest || !origins->empty())) {
    co_return std::unexpected("indexer outbox canonical boundary is exhausted");
  }
  uint64_t epoch_sequence = 0;
  if (!origins->empty()) {
    if (origins->back().epoch_sequence == UINT64_MAX) {
      co_return std::unexpected("indexer outbox epoch sequence is exhausted");
    }
    epoch_sequence = origins->back().epoch_sequence + 1;
  }
  IndexerOutboxEpochOrigin origin{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .epoch_sequence = epoch_sequence,
      .origin =
          IndexerOutboxOrigin{
              .protocol = version::kHogdProtocol(),
              .epoch_sequence = epoch_sequence,
              .first_height = first_height,
              .first_prev_hash = std::move(first_prev_hash),
          },
  };
  auto bytes = encode_file(origin);
  if (!bytes) {
    co_return std::unexpected(bytes.error());
  }
  auto published = publish_bytes(current_epoch_origin_filename(),
                                 std::as_bytes(std::span(*bytes)));
  if (!published) {
    committed_append_cache_.reset();
  }
  if (published && committed_append_cache_) {
    committed_append_cache_->current_origin = origin;
  }
  co_return published;
}

x23::Async<IndexerOutboxStatus<void>>
IndexerOutboxStore::initialize_committed_append_cache_unlocked() const
{
  if (committed_append_cache_) {
    co_return IndexerOutboxStatus<void>{};
  }
  ++committed_history_scan_count_;

  auto records = enumerate_committed_records_raw();
  if (!records) {
    co_return std::unexpected(records.error());
  }
  auto origins = enumerate_epoch_origins_raw();
  if (!origins) {
    co_return std::unexpected(origins.error());
  }
  auto current_origin = load_current_epoch_origin();
  if (!current_origin) {
    co_return std::unexpected(current_origin.error());
  }

  const std::string current_protocol(version::kHogdProtocol());
  std::unordered_set<std::string> seen_protocols;
  std::optional<size_t> current_origin_index;
  for (size_t i = 0; i < origins->size(); ++i) {
    const auto &origin = (*origins)[i];
    if (origin.epoch_sequence != i) {
      co_return std::unexpected(
          "indexer outbox epoch sequence is discontinuous");
    }
    if (!seen_protocols.insert(origin.protocol).second) {
      co_return std::unexpected("duplicate indexer outbox epoch origin");
    }
    if (origin.protocol == current_protocol) {
      current_origin_index = i;
    }
  }
  if (current_origin_index && *current_origin_index + 1 != origins->size()) {
    co_return std::unexpected(
        "archival outbox epoch appears after current epoch");
  }
  if (current_origin->has_value() != current_origin_index.has_value()) {
    co_return std::unexpected(
        "current indexer outbox epoch origin metadata is inconsistent");
  }
  if (*current_origin) {
    const auto &info = (*origins)[*current_origin_index];
    if ((*current_origin)->epoch_sequence != info.epoch_sequence ||
        (*current_origin)->origin.first_height != info.first_height ||
        (*current_origin)->origin.first_prev_hash != info.first_prev_hash) {
      co_return std::unexpected(
          "current indexer outbox epoch origin payload does not match header");
    }
  }

  size_t record_index = 0;
  std::optional<IndexerOutboxCommittedRecordInfo> previous;
  std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary;
  bool boundary_exhausted = false;
  for (const auto &origin : *origins) {
    if (boundary_exhausted) {
      co_return std::unexpected(
          "indexer outbox epoch origin follows exhausted canonical tail");
    }
    if (canonical_boundary &&
        (origin.first_height != canonical_boundary->height ||
         origin.first_prev_hash != canonical_boundary->consensus_hash)) {
      co_return std::unexpected(
          "indexer outbox epoch origin does not continue canonical tail");
    }
    if (!canonical_boundary) {
      canonical_boundary = IndexerOutboxCanonicalBoundary{
          .height = origin.first_height,
          .consensus_hash = origin.first_prev_hash,
      };
    }

    bool first_record = true;
    while (record_index < records->size() &&
           (*records)[record_index].protocol == origin.protocol) {
      const auto &record = (*records)[record_index];
      if ((first_record && (record.block_height != origin.first_height ||
                            record.prev_hash != origin.first_prev_hash)) ||
          (!first_record &&
           (previous->block_height == INT64_MAX ||
            record.block_height != previous->block_height + 1 ||
            record.prev_hash != previous->consensus_hash))) {
        co_return std::unexpected("committed outbox chain is discontinuous");
      }
      if (record.protocol == current_protocol) {
        auto payload = co_await load_committed(record.block_height);
        if (!payload) {
          co_return std::unexpected(
              std::format("invalid current committed outbox record at {}: {}",
                          record.block_height, payload.error()));
        }
        if (sha256(payload->consensus_value) != record.consensus_hash ||
            payload->consensus_value.prev_hash != record.prev_hash) {
          co_return std::unexpected(
              "current committed outbox header/payload metadata mismatch");
        }
      }
      previous = record;
      if (record.block_height == INT64_MAX) {
        canonical_boundary.reset();
        boundary_exhausted = true;
      }
      else {
        canonical_boundary = IndexerOutboxCanonicalBoundary{
            .height = record.block_height + 1,
            .consensus_hash = record.consensus_hash,
        };
      }
      first_record = false;
      ++record_index;
    }
  }
  if (record_index != records->size()) {
    co_return std::unexpected(
        "committed outbox records do not follow epoch origin order");
  }

  committed_append_cache_ = CommittedAppendCache{
      .latest = std::move(previous),
      .current_origin = std::move(*current_origin),
      .canonical_boundary = std::move(canonical_boundary),
  };
  co_return IndexerOutboxStatus<void>{};
}

x23::Async<void>
IndexerOutboxStore::cache_validated_committed_append_tip(
    std::optional<IndexerOutboxCommittedRecordInfo> latest,
    std::optional<IndexerOutboxEpochOrigin> current_origin,
    std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary) const
{
  x23::CoroGuard publication_guard;
  co_await publication_guard(*committed_publication_lock_);
  committed_append_cache_ = CommittedAppendCache{
      .latest = std::move(latest),
      .current_origin = std::move(current_origin),
      .canonical_boundary = std::move(canonical_boundary),
  };
  co_return;
}

x23::Async<size_t>
IndexerOutboxStore::committed_history_scan_count_for_test() const
{
  x23::CoroGuard publication_guard;
  co_await publication_guard(*committed_publication_lock_);
  co_return committed_history_scan_count_;
}

IndexerOutboxStatus<bool>
IndexerOutboxStore::has_committed(BlockHeight block_height) const
{
  if (block_height < 0) {
    return false;
  }
  return has_regular_file(committed_filename(block_height));
}

IndexerOutboxStatus<std::vector<BlockHeight>>
IndexerOutboxStore::enumerate_committed_heights_raw() const
{
  auto records = enumerate_committed_records_raw();
  if (!records) {
    return std::unexpected(records.error());
  }
  std::vector<BlockHeight> heights;
  heights.reserve(records->size());
  for (const auto &record : *records) {
    heights.push_back(record.block_height);
  }
  return heights;
}

IndexerOutboxStatus<std::optional<IndexerOutboxCommittedRecordInfo>>
IndexerOutboxStore::latest_committed_record_raw() const
{
  auto records = enumerate_committed_records_raw();
  if (!records) {
    return std::unexpected(records.error());
  }
  if (records->empty()) {
    return std::optional<IndexerOutboxCommittedRecordInfo>{};
  }
  return std::optional<IndexerOutboxCommittedRecordInfo>(
      std::move(records->back()));
}

IndexerOutboxStatus<std::vector<IndexerOutboxCommittedRecordInfo>>
IndexerOutboxStore::enumerate_committed_records_raw() const
{
  auto names = list_names();
  if (!names) {
    return std::unexpected(names.error());
  }

  std::vector<IndexerOutboxCommittedRecordInfo> records;
  for (const std::string &name : *names) {
    auto height = parse_filename_identity<BlockHeight>(name, kCommittedPrefix);
    if (!height) {
      continue;
    }
    auto header = load_header(name);
    if (!header) {
      return std::unexpected(header.error());
    }
    if (header->record_identity.kind() !=
            IndexerOutboxRecordKind::IndexerOutboxCommitted ||
        header->block_height != *height) {
      return std::unexpected("committed outbox filename/header mismatch");
    }
    if (auto valid = validate_payload_digest(name, *header); !valid) {
      return std::unexpected(valid.error());
    }
    records.push_back({.block_height = header->block_height,
                       .protocol = std::string(header->protocol),
                       .payload_byte_length = header->payload_byte_length,
                       .payload_digest = header->payload_digest,
                       .consensus_hash = header->consensus_hash,
                       .prev_hash = header->prev_hash});
  }
  std::ranges::sort(records, [](const auto &lhs, const auto &rhs) {
    return lhs.block_height < rhs.block_height;
  });
  return records;
}

IndexerOutboxStatus<std::vector<IndexerOutboxEpochOriginInfo>>
IndexerOutboxStore::enumerate_epoch_origins_raw() const
{
  auto names = list_names();
  if (!names) {
    return std::unexpected(names.error());
  }

  std::vector<IndexerOutboxEpochOriginInfo> origins;
  for (const std::string &name : *names) {
    if (!is_epoch_origin_filename(name)) {
      continue;
    }
    auto header = load_header(name);
    if (!header) {
      return std::unexpected(header.error());
    }
    if (header->record_identity.kind() !=
            IndexerOutboxRecordKind::IndexerOutboxEpochOrigin ||
        name != epoch_origin_filename(header->protocol)) {
      return std::unexpected("epoch origin filename/header mismatch");
    }
    if (auto valid = validate_payload_digest(name, *header); !valid) {
      return std::unexpected(valid.error());
    }
    origins.push_back({.protocol = std::string(header->protocol),
                       .epoch_sequence = header->epoch_sequence,
                       .first_height = header->block_height,
                       .first_prev_hash = header->prev_hash,
                       .payload_byte_length = header->payload_byte_length,
                       .payload_digest = header->payload_digest});
  }
  std::ranges::sort(origins, [](const auto &lhs, const auto &rhs) {
    return std::tie(lhs.epoch_sequence, lhs.protocol) <
           std::tie(rhs.epoch_sequence, rhs.protocol);
  });
  return origins;
}

IndexerOutboxStatus<void>
IndexerOutboxStore::cleanup_stale_temp_files()
{
  auto names = list_names();
  if (!names) {
    return std::unexpected(names.error());
  }

  bool removed_any = false;
  for (const std::string &name : *names) {
    if (!is_recognized_temp_filename(name)) {
      continue;
    }
    recognized_residue_on_open_ = true;
    struct stat stat_buf{};
    if (fstatat(directory_fd_.get(), name.c_str(), &stat_buf,
                AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        continue;
      }
      return std::unexpected(
          system_error("stat stale outbox temp file", errno));
    }
    if (!S_ISREG(stat_buf.st_mode)) {
      continue;
    }
    if (unlinkat(directory_fd_.get(), name.c_str(), 0) != 0) {
      return std::unexpected(
          system_error("remove stale outbox temp file", errno));
    }
    removed_any = true;
  }
  if (removed_any && fsync(directory_fd_.get()) != 0) {
    return std::unexpected(
        system_error("fsync indexer outbox directory after cleanup", errno));
  }
  return {};
}

IndexerOutboxStatus<bool>
IndexerOutboxStore::recognized_namespace_empty() const
{
  if (recognized_residue_on_open_) {
    return false;
  }
  auto names = list_names();
  if (!names) {
    return std::unexpected(names.error());
  }
  return std::ranges::none_of(*names, [](const std::string &name) {
    return is_recognized_final_filename(name) ||
           is_recognized_temp_filename(name);
  });
}

IndexerOutboxStatus<std::vector<std::string>>
IndexerOutboxStore::list_names() const
{
  const int scan_fd = openat(directory_fd_.get(), ".",
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (scan_fd < 0) {
    return std::unexpected(system_error("open indexer outbox scan fd", errno));
  }
  DIR *dir = fdopendir(scan_fd);
  if (dir == nullptr) {
    const int error = errno;
    std::ignore = close(scan_fd);
    return std::unexpected(
        system_error("open indexer outbox directory stream", error));
  }
  Defer close_dir([&] { std::ignore = closedir(dir); });

  std::vector<std::string> names;
  errno = 0;
  while (struct dirent *entry = readdir(dir)) {
    std::string_view name(entry->d_name);
    if (name != "." && name != "..") {
      names.emplace_back(name);
    }
    errno = 0;
  }
  if (errno != 0) {
    return std::unexpected(
        system_error("read indexer outbox directory", errno));
  }
  return names;
}

IndexerOutboxStatus<void>
IndexerOutboxStore::publish_bytes(std::string_view filename,
                                  std::span<const std::byte> bytes) const
{
  if (auto valid = validate_indexer_outbox_file_size(bytes.size()); !valid) {
    return valid;
  }

  static std::atomic<uint64_t> sequence = 0;
  std::string temp_filename;
  UniqueFd temp_fd;
  for (size_t attempt = 0; attempt < 100; ++attempt) {
    temp_filename = std::format("{}{}.{}.{}", filename, kTempMarker, getpid(),
                                sequence.fetch_add(1));
    const int fd =
        openat(directory_fd_.get(), temp_filename.c_str(),
               O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      temp_fd.reset(fd);
      break;
    }
    if (errno != EEXIST) {
      return std::unexpected(
          system_error("create indexer outbox temp file", errno));
    }
  }
  if (!temp_fd) {
    return std::unexpected("could not create unique indexer outbox temp file");
  }
  Defer cleanup([&] {
    std::ignore = unlinkat(directory_fd_.get(), temp_filename.c_str(), 0);
  });

  File temp_file(temp_fd.release());
  if (auto written = write_full(temp_file, bytes); !written) {
    return std::unexpected(
        std::format("write indexer outbox temp file: {}", written.error()));
  }
  if (auto synced = temp_file.sync(); !synced) {
    return std::unexpected(
        std::format("fsync indexer outbox temp file: {}", synced.error()));
  }

  const std::string final_filename(filename);
  bool linked = false;
  if (linkat(directory_fd_.get(), temp_filename.c_str(), directory_fd_.get(),
             final_filename.c_str(), 0) == 0) {
    linked = true;
    if (consume_fault(IndexerOutboxFaultPoint::kVerifyPublishedInode)) {
      return std::unexpected("injected verify published indexer outbox inode");
    }
    struct stat temp_stat{};
    struct stat final_stat{};
    if (fstat(temp_file.get_fd(), &temp_stat) != 0 ||
        fstatat(directory_fd_.get(), final_filename.c_str(), &final_stat,
                AT_SYMLINK_NOFOLLOW) != 0) {
      return std::unexpected(
          system_error("verify published indexer outbox inode", errno));
    }
    if (!S_ISREG(final_stat.st_mode) || !same_inode(temp_stat, final_stat)) {
      return std::unexpected("published indexer outbox inode changed");
    }
  }
  else if (errno == EEXIST) {
    auto existing = load_bytes(filename);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    if (!std::ranges::equal(std::as_bytes(std::span(*existing)), bytes)) {
      return std::unexpected(
          "refusing to replace mismatched indexer outbox file");
    }
  }
  else {
    return std::unexpected(system_error("publish indexer outbox file", errno));
  }

  if (linked && consume_fault(IndexerOutboxFaultPoint::kClosePublishedTemp)) {
    return std::unexpected("injected close indexer outbox temp file");
  }
  if (auto closed = temp_file.close(); !closed) {
    return std::unexpected(
        std::format("close indexer outbox temp file: {}", closed.error()));
  }
  if (linked && consume_fault(IndexerOutboxFaultPoint::kUnlinkPublishedTemp)) {
    return std::unexpected("injected remove indexer outbox temp file");
  }
  if (unlinkat(directory_fd_.get(), temp_filename.c_str(), 0) != 0) {
    return std::unexpected(
        system_error("remove indexer outbox temp file", errno));
  }
  std::ignore = cleanup.release();
  if (consume_fault(IndexerOutboxFaultPoint::kFsyncPublishedDirectory)) {
    return std::unexpected("injected fsync indexer outbox directory");
  }
  if (fsync(directory_fd_.get()) != 0) {
    return std::unexpected(
        system_error("fsync indexer outbox directory", errno));
  }
  return {};
}

IndexerOutboxStatus<void>
IndexerOutboxStore::replace_bytes(std::string_view filename,
                                  std::span<const std::byte> bytes) const
{
  if (auto valid = validate_indexer_outbox_file_size(bytes.size()); !valid) {
    return valid;
  }

  static std::atomic<uint64_t> sequence = 0;
  std::string temp_filename;
  UniqueFd temp_fd;
  for (size_t attempt = 0; attempt < 100; ++attempt) {
    temp_filename = std::format("{}{}.{}.{}", filename, kTempMarker, getpid(),
                                sequence.fetch_add(1));
    const int fd =
        openat(directory_fd_.get(), temp_filename.c_str(),
               O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      temp_fd.reset(fd);
      break;
    }
    if (errno != EEXIST) {
      return std::unexpected(
          system_error("create indexer slot temp file", errno));
    }
  }
  if (!temp_fd) {
    return std::unexpected("could not create unique indexer slot temp file");
  }
  Defer cleanup([&] {
    std::ignore = unlinkat(directory_fd_.get(), temp_filename.c_str(), 0);
  });

  File temp_file(temp_fd.release());
  if (auto written = write_full(temp_file, bytes); !written) {
    return std::unexpected(
        std::format("write indexer slot temp file: {}", written.error()));
  }
  if (auto synced = temp_file.sync(); !synced) {
    return std::unexpected(
        std::format("fsync indexer slot temp file: {}", synced.error()));
  }
  if (auto closed = temp_file.close(); !closed) {
    return std::unexpected(
        std::format("close indexer slot temp file: {}", closed.error()));
  }

  const std::string final_filename(filename);
  if (renameat(directory_fd_.get(), temp_filename.c_str(), directory_fd_.get(),
               final_filename.c_str()) != 0) {
    return std::unexpected(system_error("replace durable indexer slot", errno));
  }
  std::ignore = cleanup.release();
  if (consume_fault(IndexerOutboxFaultPoint::kFsyncPublishedDirectory)) {
    return std::unexpected("injected fsync indexer slot directory");
  }
  if (fsync(directory_fd_.get()) != 0) {
    return std::unexpected(
        system_error("fsync directory after indexer slot replacement", errno));
  }
  return {};
}

IndexerOutboxStatus<std::vector<uint8_t>>
IndexerOutboxStore::load_bytes(std::string_view filename) const
{
  const std::string name(filename);
  const int fd = openat(directory_fd_.get(), name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return std::unexpected(system_error("open indexer outbox file", errno));
  }
  File file(fd);

  struct stat file_stat{};
  struct stat path_stat{};
  if (fstat(file.get_fd(), &file_stat) != 0 ||
      fstatat(directory_fd_.get(), name.c_str(), &path_stat,
              AT_SYMLINK_NOFOLLOW) != 0) {
    return std::unexpected(system_error("stat indexer outbox file", errno));
  }
  if (!S_ISREG(file_stat.st_mode) || !S_ISREG(path_stat.st_mode) ||
      !same_inode(file_stat, path_stat)) {
    return std::unexpected("indexer outbox path is not a stable regular file");
  }
  if (file_stat.st_size < 0) {
    return std::unexpected("indexer outbox file has negative size");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);
  if (file_size > std::numeric_limits<size_t>::max()) {
    return std::unexpected("indexer outbox file size does not fit size_t");
  }
  if (auto valid =
          validate_indexer_outbox_file_size(static_cast<size_t>(file_size));
      !valid) {
    return std::unexpected(valid.error());
  }

  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  if (auto read = read_full(file, std::as_writable_bytes(std::span(bytes)));
      !read) {
    return std::unexpected(
        std::format("read indexer outbox file: {}", read.error()));
  }
  std::byte extra;
  auto extra_read = file.read(std::span(&extra, 1));
  if (!extra_read) {
    return std::unexpected(std::format("finish reading indexer outbox file: {}",
                                       extra_read.error()));
  }
  if (*extra_read != 0) {
    return std::unexpected("indexer outbox file changed while being read");
  }
  return bytes;
}

IndexerOutboxStatus<IndexerOutboxRecordHeader>
IndexerOutboxStore::load_header(std::string_view filename) const
{
  const std::string name(filename);
  const int fd = openat(directory_fd_.get(), name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return std::unexpected(system_error("open indexer outbox header", errno));
  }
  UniqueFd file(fd);
  struct stat file_stat{};
  struct stat path_stat{};
  if (fstat(file.get(), &file_stat) != 0 ||
      fstatat(directory_fd_.get(), name.c_str(), &path_stat,
              AT_SYMLINK_NOFOLLOW) != 0) {
    return std::unexpected(system_error("stat indexer outbox header", errno));
  }
  if (!S_ISREG(file_stat.st_mode) || !S_ISREG(path_stat.st_mode) ||
      !same_inode(file_stat, path_stat) || file_stat.st_size < 0) {
    return std::unexpected("indexer outbox header path is not stable");
  }
  const auto file_size = static_cast<uint64_t>(file_stat.st_size);
  if (file_size < 4) {
    return std::unexpected("truncated indexer outbox header length");
  }
  std::array<uint8_t, 4> length_bytes{};
  if (pread(file.get(), length_bytes.data(), length_bytes.size(), 0) != 4) {
    return std::unexpected("failed to read indexer outbox header length");
  }
  uint32_t encoded_header_size = 0;
  try {
    x23::deserialize(&encoded_header_size, x23::BufView(length_bytes));
  } catch (const x23::XdrException &e) {
    return std::unexpected(
        std::format("malformed indexer outbox header length: {}", e.what()));
  }
  if (encoded_header_size > kMaxIndexerOutboxHeaderBytes ||
      file_size < 4 + encoded_header_size) {
    return std::unexpected("invalid bounded indexer outbox header size");
  }
  std::vector<uint8_t> prefix(encoded_header_size);
  size_t offset = 0;
  // NOLINTNEXTLINE(misc-include-cleaner)
  off_t file_offset = 4;
  while (offset < prefix.size()) {
    const ssize_t read_size = pread(file.get(), prefix.data() + offset,
                                    prefix.size() - offset, file_offset);
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(system_error("read indexer outbox header", errno));
    }
    if (read_size == 0) {
      return std::unexpected("truncated indexer outbox header");
    }
    offset += static_cast<size_t>(read_size);
    file_offset += read_size;
  }

  IndexerOutboxRecordHeader header;
  try {
    x23::deserialize(&header, x23::BufView(prefix));
  } catch (const x23::XdrException &e) {
    return std::unexpected(
        std::format("malformed indexer outbox header XDR: {}", e.what()));
  }
  const size_t header_size = x23::xdr_size(header);
  if (header_size != encoded_header_size) {
    return std::unexpected("invalid bounded indexer outbox header size");
  }
  if (header.format_version != kIndexerOutboxEnvelopeFormatVersion) {
    return std::unexpected("unknown indexer outbox header version");
  }
  if (header.block_height < 0 ||
      header.payload_byte_length > kMaxIndexerOutboxPayloadBytes ||
      header.payload_byte_length != file_size - 4 - header_size) {
    return std::unexpected("indexer outbox header/file size mismatch");
  }
  return header;
}

IndexerOutboxStatus<std::vector<uint8_t>>
IndexerOutboxStore::load_payload(std::string_view filename,
                                 const IndexerOutboxRecordHeader &header) const
{
  const std::string name(filename);
  const int fd = openat(directory_fd_.get(), name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return std::unexpected(system_error("open indexer outbox payload", errno));
  }
  UniqueFd file(fd);
  struct stat file_stat{};
  if (fstat(file.get(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
      file_stat.st_size < 0) {
    return std::unexpected(system_error("stat indexer outbox payload", errno));
  }
  const size_t header_size = x23::xdr_size(header);
  const uint64_t expected_size = 4 + header_size + header.payload_byte_length;
  if (std::cmp_not_equal(file_stat.st_size, expected_size)) {
    return std::unexpected("indexer outbox file changed after header read");
  }
  std::vector<uint8_t> payload(static_cast<size_t>(header.payload_byte_length));
  size_t offset = 0;
  // NOLINTNEXTLINE(misc-include-cleaner)
  off_t file_offset = 4;
  file_offset += static_cast<off_t>(header_size);
  while (offset < payload.size()) {
    const ssize_t read_size = pread(file.get(), payload.data() + offset,
                                    payload.size() - offset, file_offset);
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(
          system_error("read indexer outbox payload", errno));
    }
    if (read_size == 0) {
      return std::unexpected("truncated indexer outbox payload");
    }
    offset += static_cast<size_t>(read_size);
    file_offset += read_size;
  }
  if (sha256(x23::BufView(payload)) != header.payload_digest) {
    return std::unexpected("indexer outbox payload digest mismatch");
  }
  return payload;
}

IndexerOutboxStatus<void>
IndexerOutboxStore::validate_payload_digest(
    std::string_view filename, const IndexerOutboxRecordHeader &header) const
{
  const std::string name(filename);
  const int fd = openat(directory_fd_.get(), name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return std::unexpected(system_error("open indexer outbox payload", errno));
  }
  UniqueFd file(fd);
  struct stat file_stat{};
  struct stat path_stat{};
  if (fstat(file.get(), &file_stat) != 0 ||
      fstatat(directory_fd_.get(), name.c_str(), &path_stat,
              AT_SYMLINK_NOFOLLOW) != 0) {
    return std::unexpected(system_error("stat indexer outbox payload", errno));
  }
  const size_t header_size = x23::xdr_size(header);
  const uint64_t expected_size = 4 + header_size + header.payload_byte_length;
  if (!S_ISREG(file_stat.st_mode) || !S_ISREG(path_stat.st_mode) ||
      !same_inode(file_stat, path_stat) || file_stat.st_size < 0 ||
      std::cmp_not_equal(file_stat.st_size, expected_size)) {
    return std::unexpected("indexer outbox payload path is not stable");
  }

  SHA256Hasher hasher;
  std::array<uint8_t, 64 * 1024> buffer{};
  uint64_t offset = 0;
  while (offset < header.payload_byte_length) {
    const size_t chunk_size = static_cast<size_t>(
        std::min<uint64_t>(buffer.size(), header.payload_byte_length - offset));
    const ssize_t read_size =
        pread(file.get(), buffer.data(), chunk_size,
              static_cast<off_t>(4 + header_size + offset));
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(
          system_error("read indexer outbox payload", errno));
    }
    if (read_size == 0) {
      return std::unexpected("truncated indexer outbox payload");
    }
    hasher.update(std::as_bytes(
        std::span(buffer.data(), static_cast<size_t>(read_size))));
    offset += static_cast<size_t>(read_size);
  }
  if (hasher.finalize() != header.payload_digest) {
    return std::unexpected("indexer outbox payload digest mismatch");
  }
  return {};
}

IndexerOutboxStatus<bool>
IndexerOutboxStore::has_regular_file(std::string_view filename) const
{
  const std::string name(filename);
  struct stat stat_buf{};
  if (fstatat(directory_fd_.get(), name.c_str(), &stat_buf,
              AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return false;
    }
    return std::unexpected(system_error("stat indexer outbox file", errno));
  }
  if (!S_ISREG(stat_buf.st_mode)) {
    return std::unexpected("indexer outbox path is not a regular file");
  }
  return true;
}

} // namespace hogd
