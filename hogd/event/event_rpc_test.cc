#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono> // IWYU pragma: keep
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_conflict.hh"
#include "event.hh"
#include "event_outbox_internal.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/indexer_outbox.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_indexer.h"
#include "hogd/event/rpc_from_sched.h"
#include "hogd/event/status.h"
#include "hogd/event/test_util/socketpair_indexer_client.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/fileio.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

class EventRpcTest : public ::testing::Test {
public:
  EventRpcTest() = default;
  void SetUp() override { temp_dir_ = std::make_unique<TmpDir>(); }
  void TearDown() override { temp_dir_.reset(); }

  std::string temp_dir() const { return temp_dir_->directory(); }

private:
  std::unique_ptr<TmpDir> temp_dir_;
};

namespace {

ProposedTransaction
make_body(uint64_t nonce)
{
  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.block_expiry_height = 1000 + nonce;
  body.signed_tx.tx.uniquifier.nonce = nonce;
  body.signed_tx.tx.invocation.gas_limit = 1'000'000 + nonce;
  body.nondeterministic_proposal_data.nondeterministic_bits.push_back(
      static_cast<uint8_t>(nonce));
  body.nondeterministic_proposal_data.nondeterministic_bit_count = 8;
  return body;
}

TxEventLog
make_event(const ProposedTransaction &body, uint8_t marker)
{
  TxEventLog event;
  event.proposed_tx_hash = sha256(body.signed_tx.tx);
  TxEmittedEvent emitted;
  emitted.contract_address[0] = marker;
  emitted.serialized_event.push_back(marker);
  event.events.push_back(std::move(emitted));
  return event;
}

ProposalTxs
make_proposal_txs(const ProposedTransactionBatch &bodies)
{
  x23::XdrVec<SignedTransactionHash> hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  for (const ProposedTransaction &body : bodies) {
    hashes.push_back(sha256(body.signed_tx.tx));
    nondeterministic_data.push_back(body.nondeterministic_proposal_data);
  }
  ProposalTxs proposal_txs;
  append_to_parallel_batch(x23::span<const SignedTransactionHash>(hashes),
                           proposal_txs.hash)();
  append_to_parallel_batch(x23::span<const NondeterministicProposalData>(
                               nondeterministic_data),
                           proposal_txs.nondeterministic_data)();
  return proposal_txs;
}

ConsensusValue
make_consensus_value(BlockHeight height, SHA256Hash event_root,
                     const ProposalTxs &proposal_txs)
{
  ConsensusValue value{};
  value.proposal_header.block_height = height;
  value.proposal_header.events_hash = event_root;
  auto txs_root = compute_indexer_proposal_txs_root(proposal_txs);
  CHECK(txs_root.has_value());
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal_txs);
  return value;
}

IndexerOutboxCommittedPayload
make_empty_committed(BlockID /*commit_alias*/, BlockHeight height,
                     ConsensusValueHash prev_hash)
{
  ProposalTxs proposal_txs;
  TxEventLogBatch event_logs;
  ConsensusValue value =
      make_consensus_value(height,
                           compute_indexer_outbox_event_root(event_logs),
                           proposal_txs);
  value.prev_hash = std::move(prev_hash);
  return {.format_version = kIndexerOutboxFormatVersion,
          .consensus_value = std::move(value),
          .proposal_txs = std::move(proposal_txs),
          .event_logs = std::move(event_logs),
          .tx_bodies = {}};
}

void
initialize_test_origin(EventRPCContext &ctx, const ConsensusValue &first)
{
  ASSERT_TRUE(
      ctx.indexer_outbox_store()
          .initialize_current_epoch_origin(first.proposal_header.block_height,
                                           first.prev_hash)
          .has_value());
}

template<typename T>
void
expect_xdr_equal(const T &lhs, const T &rhs)
{
  EXPECT_EQ(x23::serialize(lhs), x23::serialize(rhs));
}

void
rewrite_outbox_protocol(std::string_view directory, std::string_view filename,
                        std::string_view protocol)
{
  const std::filesystem::path path =
      std::filesystem::path(directory) / "indexer-outbox" / filename;
  auto bytes = x23::readfile<x23::Buf>(path.string());
  ASSERT_TRUE(bytes.has_value());
  uint32_t header_size = 0;
  x23::deserialize(&header_size, x23::BufView(bytes->data(), size_t{4}));
  IndexerOutboxRecordHeader header;
  x23::deserialize(&header, x23::BufView(bytes->data() + 4, header_size));
  header.protocol = ProtocolCommit(protocol.data(), protocol.size());
  x23::Buf header_bytes = x23::serialize(header);
  x23::Buf rewritten =
      x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  rewritten.append_range(header_bytes);
  rewritten.append_range(std::span(bytes->data() + 4 + header_size,
                                   bytes->size() - 4 - header_size));
  auto file = File::open(path.string(), O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(
      write_full(*file, std::as_bytes(std::span(rewritten))).has_value());
  ASSERT_TRUE(file->close().has_value());
}

void
corrupt_outbox_payload(std::string_view directory, std::string_view filename)
{
  const std::filesystem::path path =
      std::filesystem::path(directory) / "indexer-outbox" / filename;
  auto bytes = x23::readfile<x23::Buf>(path.string());
  ASSERT_TRUE(bytes.has_value());
  uint32_t header_size = 0;
  x23::deserialize(&header_size, x23::BufView(bytes->data(), size_t{4}));
  ASSERT_GT(bytes->size(), 4 + header_size);
  bytes->back() ^= 0x01;
  auto file = File::open(path.string(), O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(write_full(*file, std::as_bytes(std::span(*bytes))).has_value());
  ASSERT_TRUE(file->close().has_value());
}

void
replace_outbox_payload(std::string_view directory, std::string_view filename,
                       x23::Buf payload)
{
  const std::filesystem::path path =
      std::filesystem::path(directory) / "indexer-outbox" / filename;
  auto bytes = x23::readfile<x23::Buf>(path.string());
  ASSERT_TRUE(bytes.has_value());
  uint32_t header_size = 0;
  x23::deserialize(&header_size, x23::BufView(bytes->data(), size_t{4}));
  IndexerOutboxRecordHeader header;
  x23::deserialize(&header, x23::BufView(bytes->data() + 4, header_size));
  header.payload_byte_length = payload.size();
  header.payload_digest = sha256(x23::BufView(payload));
  x23::Buf header_bytes = x23::serialize(header);
  x23::Buf rewritten =
      x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  rewritten.append_range(header_bytes);
  rewritten.append_range(payload);
  auto file = File::open(path.string(), O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(
      write_full(*file, std::as_bytes(std::span(rewritten))).has_value());
  ASSERT_TRUE(file->close().has_value());
}

void
rewrite_committed_prev_hash(std::string_view directory,
                            std::string_view filename,
                            ConsensusValueHash prev_hash)
{
  const std::filesystem::path path =
      std::filesystem::path(directory) / "indexer-outbox" / filename;
  auto bytes = x23::readfile<x23::Buf>(path.string());
  ASSERT_TRUE(bytes.has_value());
  uint32_t header_size = 0;
  x23::deserialize(&header_size, x23::BufView(bytes->data(), size_t{4}));
  IndexerOutboxRecordHeader header;
  x23::deserialize(&header, x23::BufView(bytes->data() + 4, header_size));
  IndexerOutboxCommittedPayload payload;
  x23::deserialize(&payload, x23::BufView(bytes->data() + 4 + header_size,
                                          bytes->size() - 4 - header_size));
  payload.consensus_value.prev_hash = std::move(prev_hash);

  x23::Buf payload_bytes = x23::serialize(payload);
  header.payload_byte_length = payload_bytes.size();
  header.payload_digest = sha256(x23::BufView(payload_bytes));
  header.consensus_hash = sha256(payload.consensus_value);
  header.prev_hash = payload.consensus_value.prev_hash;
  x23::Buf header_bytes = x23::serialize(header);
  x23::Buf rewritten =
      x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  rewritten.append_range(header_bytes);
  rewritten.append_range(payload_bytes);
  auto file = File::open(path.string(), O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(
      write_full(*file, std::as_bytes(std::span(rewritten))).has_value());
  ASSERT_TRUE(file->close().has_value());
}

void
write_epoch_origin_header(std::string_view directory, std::string_view protocol,
                          BlockHeight first_height,
                          ConsensusValueHash first_prev_hash,
                          uint64_t epoch_sequence = 0)
{
  IndexerOutboxEpochOrigin origin{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .epoch_sequence = epoch_sequence,
      .origin =
          IndexerOutboxOrigin{
              .protocol = ProtocolCommit(protocol.data(), protocol.size()),
              .epoch_sequence = epoch_sequence,
              .first_height = first_height,
              .first_prev_hash = first_prev_hash,
          },
  };
  x23::Buf payload = x23::serialize(origin);
  IndexerOutboxRecordHeader header{
      .format_version = kIndexerOutboxEnvelopeFormatVersion,
      .epoch_sequence = epoch_sequence,
      .block_height = first_height,
      .protocol = ProtocolCommit(protocol.data(), protocol.size()),
      .payload_byte_length = payload.size(),
      .payload_digest = sha256(x23::BufView(payload)),
      .prev_hash = std::move(first_prev_hash),
  };
  header.record_identity.kind(
      IndexerOutboxRecordKind::IndexerOutboxEpochOrigin);
  x23::Buf header_bytes = x23::serialize(header);
  x23::Buf bytes = x23::serialize(static_cast<uint32_t>(header_bytes.size()));
  bytes.append_range(header_bytes);
  bytes.append_range(payload);
  const std::filesystem::path path =
      std::filesystem::path(directory) / "indexer-outbox" /
      IndexerOutboxStore::epoch_origin_filename(protocol);
  auto file = File::open(path.string(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(write_full(*file, std::as_bytes(std::span(bytes))).has_value());
  ASSERT_TRUE(file->close().has_value());
}

// Minimal IndexerEvent2 fake capturing failure reports and strict committed
// block applications.
struct FailureCapturingIndexerRpc {
  using rpc_service = IndexerEvent2;

  x23::Async<IndexerProgress> indexer_event_get_progress()
  {
    co_return progress;
  }

  x23::Async<IndexerEpochResult>
  indexer_event_initialize_epoch(const IndexerOutboxOrigin &origin)
  {
    IndexerInitializedProgress initialized{
        .origin = origin,
        .contiguous_height = origin.first_height - 1,
        .last_consensus_hash = origin.first_prev_hash,
        .last_payload_digest = {},
    };
    if (progress.state() == IndexerProgressState::IndexerProgressInitialized) {
      initialized.contiguous_height = progress.initialized().contiguous_height;
      initialized.last_consensus_hash =
          progress.initialized().last_consensus_hash;
      initialized.last_payload_digest =
          progress.initialized().last_payload_digest;
    }
    progress.state(IndexerProgressState::IndexerProgressInitialized);
    progress.initialized() = initialized;
    IndexerEpochResult result(IndexerEpochOutcome::IndexerEpochInitialized);
    result.progress() = initialized;
    co_return result;
  }

  x23::Async<IndexerBlockApplyResult>
  indexer_event_apply_committed_block(const IndexerOutboxOrigin &origin,
                                      const IndexerCommittedBlock &block)
  {
    const BlockHeight block_height =
        block.payload.consensus_value.proposal_header.block_height;
    event_batches.emplace_back(block_height, block.payload.event_logs.size());
    IndexerInitializedProgress initialized{
        .origin = origin,
        .contiguous_height = block_height,
        .last_consensus_hash = block.identity.consensus_hash,
        .last_payload_digest = block.identity.payload_digest,
    };
    progress.state(IndexerProgressState::IndexerProgressInitialized);
    progress.initialized() = initialized;
    IndexerBlockApplyResult result(
        IndexerBlockApplyOutcome::IndexerBlockApplied);
    result.progress() = initialized;
    co_return result;
  }

  x23::Async<void>
  indexer_event_report_tx_failure_v2(const SignedTransactionHash &tx_hash,
                                     const TxFailureDiagnostic &diag)
  {
    reports.emplace_back(tx_hash, diag);
    co_return;
  }

  std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>> reports;
  std::vector<std::pair<BlockHeight, size_t>> event_batches;
  IndexerProgress progress{IndexerProgressState::IndexerProgressUninitialized};
};
static_assert(is_IndexerEvent2<FailureCapturingIndexerRpc *, x23::Async>);

// IndexerEvent2 fake that only counts received messages, via atomics so the
// test thread can poll them without racing the RPC reader thread.
struct CountingIndexerRpc {
  using rpc_service = IndexerEvent2;

  static x23::Async<IndexerProgress> indexer_event_get_progress()
  {
    co_return IndexerProgress(
        IndexerProgressState::IndexerProgressUninitialized);
  }

  static x23::Async<IndexerEpochResult>
  indexer_event_initialize_epoch(const IndexerOutboxOrigin &origin)
  {
    IndexerEpochResult result(IndexerEpochOutcome::IndexerEpochInitialized);
    result.progress() = {
        .origin = origin,
        .contiguous_height = origin.first_height - 1,
        .last_consensus_hash = origin.first_prev_hash,
        .last_payload_digest = {},
    };
    co_return result;
  }

  x23::Async<IndexerBlockApplyResult>
  indexer_event_apply_committed_block(const IndexerOutboxOrigin &origin,
                                      const IndexerCommittedBlock &block)
  {
    events.fetch_add(1, std::memory_order_relaxed);
    IndexerBlockApplyResult result(
        IndexerBlockApplyOutcome::IndexerBlockApplied);
    result.progress() = {
        .origin = origin,
        .contiguous_height =
            block.payload.consensus_value.proposal_header.block_height,
        .last_consensus_hash = block.identity.consensus_hash,
        .last_payload_digest = block.identity.payload_digest,
    };
    co_return result;
  }

  x23::Async<void>
  indexer_event_report_tx_failure_v2(const SignedTransactionHash & /*tx_hash*/,
                                     const TxFailureDiagnostic & /*diag*/)
  {
    reports.fetch_add(1, std::memory_order_relaxed);
    co_return;
  }

  std::atomic<size_t> events = 0;
  std::atomic<size_t> reports = 0;
};
static_assert(is_IndexerEvent2<CountingIndexerRpc *, x23::Async>);

struct BlockingIndexerRpc : FailureCapturingIndexerRpc {
  x23::Async<IndexerBlockApplyResult>
  indexer_event_apply_committed_block(const IndexerOutboxOrigin &origin,
                                      const IndexerCommittedBlock &block)
  {
    apply_started.count_down();
    release_apply.wait();
    co_return co_await FailureCapturingIndexerRpc::
        indexer_event_apply_committed_block(origin, block);
  }

  std::latch apply_started{1};
  std::latch release_apply{1};
};
static_assert(is_IndexerEvent2<BlockingIndexerRpc *, x23::Async>);

struct ForgedEpochProgressIndexerRpc : FailureCapturingIndexerRpc {
  static x23::Async<IndexerEpochResult>
  indexer_event_initialize_epoch(const IndexerOutboxOrigin &origin)
  {
    IndexerEpochResult result(IndexerEpochOutcome::IndexerEpochInitialized);
    result.progress() = {
        .origin = origin,
        .contiguous_height = origin.first_height + 100,
        .last_consensus_hash = {},
        .last_payload_digest = {},
    };
    co_return result;
  }
};
static_assert(is_IndexerEvent2<ForgedEpochProgressIndexerRpc *, x23::Async>);

// Reads exactly [n] bytes from [fd]; false on EOF/error.
bool
read_exact(int fd, void *buf, size_t n)
{
  auto *p = static_cast<uint8_t *>(buf);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r <= 0) {
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

// Drains and counts complete RPC records off [fd] without ever replying, so
// every relayed report stays pending at the client. Returns (closing [fd])
// once the socket is shut down or closed.
void
count_rpc_records(int fd, std::atomic<size_t> &records_read)
{
  for (;;) {
    uint8_t hdr[4];
    if (!read_exact(fd, hdr, sizeof(hdr))) {
      break;
    }
    const uint32_t raw = (static_cast<uint32_t>(hdr[0]) << 24) |
                         (static_cast<uint32_t>(hdr[1]) << 16) |
                         (static_cast<uint32_t>(hdr[2]) << 8) |
                         static_cast<uint32_t>(hdr[3]);
    const bool last = (raw & 0x80000000U) != 0;
    const uint32_t len = raw & 0x7fffffffU;
    uint8_t body[4096];
    if (len > sizeof(body) || (len != 0 && !read_exact(fd, body, len))) {
      break;
    }
    if (last) {
      records_read.fetch_add(1, std::memory_order_release);
    }
  }
  ::close(fd);
}

} // namespace

// Regression for #2923: an indexer peer that sends an IndexerCardV2 whose
// protocol string does not match the local hogd protocol must be rejected
// gracefully. The protocol string is remote RPC input, not an internal
// invariant, so a mismatch must not abort the event server. The mismatch is
// also detected before any network dial, so the (deliberately unreachable)
// address is never touched.
TEST_F(EventRpcTest, IndexerIntroProtocolMismatchIsRejectedNotAborted)
{
  EventRPCContext ctx(temp_dir());
  EventIndexerRpc indexer_rpc(ctx);
  const IndexerCardV2 card{
      .protocol = "definitely-not-the-hogd-protocol",
      .generation = {},
      .address_for_event = "127.0.0.1:1",
      .progress =
          IndexerProgress(IndexerProgressState::IndexerProgressUninitialized)};

  const EventIndexerRegistrationOutcome outcome =
      indexer_rpc.event_indexer_register(card)();
  EXPECT_EQ(
      outcome,
      EventIndexerRegistrationOutcome::EventRegistrationMismatchedProtocol);
}

TEST_F(EventRpcTest, IndexerRegistrationWaitsForOutboxReconciliation)
{
  EventRPCContext ctx(temp_dir());
  FailureCapturingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};

  EXPECT_EQ(
      ctx.register_indexer(make_socketpair_indexer_client(&indexer), card)(),
      EventIndexerRegistrationOutcome::EventRegistrationOriginUnavailable);
}

TEST_F(EventRpcTest, IndexerRegistrationInitializesFreshEpoch)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  FailureCapturingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  EXPECT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
  ASSERT_EQ(indexer.progress.state(),
            IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(indexer.progress.initialized().contiguous_height, 0);
  EXPECT_EQ(indexer.progress.initialized().last_consensus_hash,
            sha256(genesis));
}

TEST_F(EventRpcTest, IndexerRegistrationUsesCurrentCallbackProgress)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value());
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);

  FailureCapturingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  indexer.progress.state(IndexerProgressState::IndexerProgressInitialized);
  indexer.progress.initialized() = {
      .origin = current_origin->origin,
      .contiguous_height = 0,
      .last_consensus_hash = sha256(genesis),
      .last_payload_digest = {},
  };

  EXPECT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
}

TEST_F(EventRpcTest, IndexerRegistrationRejectsBoundaryIdentityMismatch)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value());
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);

  FailureCapturingIndexerRpc indexer;
  indexer.progress.state(IndexerProgressState::IndexerProgressInitialized);
  indexer.progress.initialized() = {
      .origin = current_origin->origin,
      .contiguous_height = 0,
      .last_consensus_hash = ConsensusValueHash{},
      .last_payload_digest = {},
  };
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  EXPECT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationProgressConflict);
}

TEST_F(EventRpcTest,
       EventRestartBeforeFirstNewEpochAcceptsPriorPayloadDigestBoundary)
{
  auto archival = make_empty_committed(80, 20, ConsensusValueHash{});
  SHA256Hash archival_digest;
  {
    EventRPCContext previous(temp_dir());
    initialize_test_origin(previous, archival.consensus_value);
    ASSERT_TRUE(previous.indexer_outbox_store()
                    .publish_committed(archival)
                    .has_value());
    auto records =
        previous.indexer_outbox_store().enumerate_committed_records_raw();
    ASSERT_TRUE(records.has_value()) << records.error();
    ASSERT_EQ(records->size(), 1UL);
    archival_digest = records->front().payload_digest;
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 20,
                            archival.consensus_value.prev_hash);

  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(restarted.reconcile_outbox_tip(80, archival.consensus_value)()
                  .has_value());
  auto origins = restarted.indexer_outbox_store().enumerate_epoch_origins_raw();
  ASSERT_TRUE(origins.has_value()) << origins.error();
  ASSERT_EQ(origins->size(), 2UL);

  FailureCapturingIndexerRpc indexer;
  indexer.progress.state(IndexerProgressState::IndexerProgressInitialized);
  indexer.progress.initialized() = {
      .origin =
          IndexerOutboxOrigin{
              .protocol = "hogp/archival",
              .epoch_sequence = 0,
              .first_height = 20,
              .first_prev_hash = archival.consensus_value.prev_hash,
          },
      .contiguous_height = 20,
      .last_consensus_hash = sha256(archival.consensus_value),
      .last_payload_digest = archival_digest,
  };
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  EXPECT_EQ(restarted.register_indexer(make_socketpair_indexer_client(&indexer),
                                       card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
  EXPECT_EQ(indexer.progress.initialized().contiguous_height, 20);
  EXPECT_EQ(indexer.progress.initialized().last_payload_digest,
            archival_digest);
  EXPECT_TRUE(indexer.event_batches.empty());
}

TEST_F(EventRpcTest, DurableGenerationAllowsIdempotentRetryButNoLiveReplacement)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  FailureCapturingIndexerRpc indexer_a;
  FailureCapturingIndexerRpc indexer_b;
  FailureCapturingIndexerRpc retry_indexer;
  FailureCapturingIndexerRpc fresh_indexer;
  IndexerCardV2 card_a{.protocol = version::kHogdProtocol(),
                       .generation = {},
                       .address_for_event = {},
                       .progress = indexer_a.progress};
  card_a.generation[0] = 0xa1;

  {
    EventRPCContext first(temp_dir());
    ASSERT_TRUE(first.reconcile_outbox_tip(0, genesis)().has_value());
    ASSERT_EQ(first.register_indexer(make_socketpair_indexer_client(&indexer_a),
                                     card_a)(),
              EventIndexerRegistrationOutcome::EventRegistrationAccepted);
  }

  IndexerCardV2 fresh_card{.protocol = version::kHogdProtocol(),
                           .generation = {},
                           .address_for_event = {},
                           .progress = fresh_indexer.progress};
  fresh_card.generation[0] = 0xc3;
  IndexerCardV2 card_b{.protocol = version::kHogdProtocol(),
                       .generation = {},
                       .address_for_event = {},
                       .progress = indexer_b.progress};
  card_b.generation[0] = 0xb2;
  card_b.predecessor_generation = card_a.generation;
  {
    EventRPCContext restarted(temp_dir());
    ASSERT_TRUE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
    EXPECT_EQ(
        restarted.register_indexer(
            make_socketpair_indexer_client(&fresh_indexer), fresh_card)(),
        EventIndexerRegistrationOutcome::EventRegistrationStaleGeneration);
    ASSERT_EQ(restarted.register_indexer(
                  make_socketpair_indexer_client(&indexer_b), card_b)(),
              EventIndexerRegistrationOutcome::EventRegistrationAccepted);
    card_b.progress = retry_indexer.progress;
    EXPECT_EQ(restarted.register_indexer(
                  make_socketpair_indexer_client(&retry_indexer), card_b)(),
              EventIndexerRegistrationOutcome::EventRegistrationAccepted);
    EXPECT_EQ(
        restarted.register_indexer(
            make_socketpair_indexer_client(&fresh_indexer), fresh_card)(),
        EventIndexerRegistrationOutcome::EventRegistrationStaleGeneration);
    SignedTransactionHash tx_hash{};
    tx_hash[0] = 0x77;
    TxFailureDiagnostic diagnostic;
    diagnostic.attempt_height = 1;
    restarted.report_tx_failure_to_indexers(tx_hash, diagnostic)();
    EXPECT_TRUE(indexer_a.reports.empty());
    EXPECT_TRUE(indexer_b.reports.empty());
    EXPECT_TRUE(fresh_indexer.reports.empty());
    ASSERT_EQ(retry_indexer.reports.size(), 1UL);
  }

  EventRPCContext restarted_again(temp_dir());
  ASSERT_TRUE(restarted_again.reconcile_outbox_tip(0, genesis)().has_value());
  card_a.progress = indexer_a.progress;
  EXPECT_EQ(restarted_again.register_indexer(
                make_socketpair_indexer_client(&indexer_a), card_a)(),
            EventIndexerRegistrationOutcome::EventRegistrationStaleGeneration);
  EXPECT_EQ(restarted_again.register_indexer(
                make_socketpair_indexer_client(&indexer_b), card_b)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
}

TEST_F(EventRpcTest, RegistrationRejectsForgedInitializedEpochProgress)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ForgedEpochProgressIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  EXPECT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationProgressConflict);
}

TEST_F(EventRpcTest, RegistrationDrainsConfirmationThatRacesInitialReplay)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ASSERT_EQ(event_rpc.eventsched2_open_block(82)(), RPCError::kRPCNone);
  EventBlockResult first_closed =
      event_rpc.eventsched2_close_block(82, 1, ProposalTxs{})();
  ASSERT_EQ(first_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, first_closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(82, block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);

  BlockingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  auto registration = std::async(std::launch::async, [&] {
    return ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                card)();
  });
  indexer.apply_started.wait();

  ASSERT_EQ(event_rpc.eventsched2_open_block(83)(), RPCError::kRPCNone);
  EventBlockResult second_closed =
      event_rpc.eventsched2_close_block(83, 2, ProposalTxs{})();
  ASSERT_EQ(second_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_two =
      make_consensus_value(2, second_closed.root_hash(), ProposalTxs{});
  block_two.prev_hash = sha256(block_one);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(83, block_two)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_two)(),
            RPCError::kRPCNone);

  EXPECT_EQ(registration.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  indexer.release_apply.count_down();
  ASSERT_EQ(registration.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(registration.get(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
  ASSERT_EQ(indexer.event_batches.size(), 2UL);
  EXPECT_EQ(indexer.event_batches[0].first, 1);
  EXPECT_EQ(indexer.event_batches[1].first, 2);
}

TEST_F(EventRpcTest, LivenessProbeDoesNotWaitForDeliveryLane)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  BlockingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  card.generation[0] = 0x51;
  ASSERT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);

  ASSERT_EQ(event_rpc.eventsched2_open_block(821)(), RPCError::kRPCNone);
  EventBlockResult closed =
      event_rpc.eventsched2_close_block(821, 1, ProposalTxs{})();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(821, block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  indexer.apply_started.wait();

  auto probe = std::async(std::launch::async,
                          [&] { return ctx.probe_indexer(card.generation); });
  ASSERT_EQ(probe.wait_for(std::chrono::milliseconds(50)),
            std::future_status::ready);
  EXPECT_EQ(probe.get(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
  UPID stale_generation = card.generation;
  stale_generation[0] ^= 1;
  EXPECT_EQ(ctx.probe_indexer(stale_generation),
            EventIndexerRegistrationOutcome::EventRegistrationStaleGeneration);
  indexer.release_apply.count_down();
}

TEST_F(EventRpcTest, EnqueueAtWorkerEmptyExitStartsAReplacementWorker)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  FailureCapturingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  card.generation[0] = 0x61;
  ASSERT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);

  ASSERT_EQ(event_rpc.eventsched2_open_block(861)(), RPCError::kRPCNone);
  EventBlockResult first_closed =
      event_rpc.eventsched2_close_block(861, 1, ProposalTxs{})();
  ASSERT_EQ(first_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, first_closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);

  ASSERT_EQ(event_rpc.eventsched2_open_block(862)(), RPCError::kRPCNone);
  EventBlockResult second_closed =
      event_rpc.eventsched2_close_block(862, 2, ProposalTxs{})();
  ASSERT_EQ(second_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_two =
      make_consensus_value(2, second_closed.root_hash(), ProposalTxs{});
  block_two.prev_hash = sha256(block_one);

  std::latch worker_at_empty_exit{1};
  std::latch release_worker{1};
  ctx.set_delivery_worker_empty_test_hook([&] {
    worker_at_empty_exit.count_down();
    release_worker.wait();
  });
  ASSERT_EQ(event_rpc.eventsched2_commit_block(861, block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  worker_at_empty_exit.wait();

  ASSERT_EQ(event_rpc.eventsched2_commit_block(862, block_two)(),
            RPCError::kRPCNone);
  auto second_confirmation = std::async(std::launch::async, [&] {
    return event_rpc.eventsched2_confirm_committed_block(block_two)();
  });
  EXPECT_EQ(second_confirmation.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  release_worker.count_down();
  ASSERT_EQ(second_confirmation.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(second_confirmation.get(), RPCError::kRPCNone);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (indexer.event_batches.size() != 2) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(indexer.event_batches[0].first, 1);
  EXPECT_EQ(indexer.event_batches[1].first, 2);
}

TEST_F(EventRpcTest,
       OneAheadRecordIsWithheldUntilExactSchedulerCatchupConfirmsIdentity)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue block_one;
  {
    EventRPCContext first(temp_dir());
    EventMempoolRpc event_rpc(first);
    ASSERT_TRUE(first.reconcile_outbox_tip(0, genesis)().has_value());
    ASSERT_EQ(event_rpc.eventsched2_open_block(84)(), RPCError::kRPCNone);
    EventBlockResult closed =
        event_rpc.eventsched2_close_block(84, 1, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    block_one = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
    block_one.prev_hash = sha256(genesis);
    ASSERT_EQ(event_rpc.eventsched2_commit_block(84, block_one)(),
              RPCError::kRPCNone);
  }

  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
  FailureCapturingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = indexer.progress};
  ASSERT_EQ(restarted.register_indexer(make_socketpair_indexer_client(&indexer),
                                       card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);
  EXPECT_TRUE(indexer.event_batches.empty());

  ConsensusValue conflicting = block_one;
  conflicting.prev_hash[0] ^= 1;
  auto rejected = restarted.commit_block(84, conflicting)();
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), EventErrorCode::kBadBlockID);
  EXPECT_TRUE(indexer.event_batches.empty());

  ASSERT_TRUE(restarted.commit_block(84, block_one)().has_value());
  EXPECT_TRUE(indexer.event_batches.empty());
  ASSERT_TRUE(restarted.confirm_committed_block(block_one)().has_value());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (indexer.event_batches.empty()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(indexer.event_batches.size(), 1UL);
  EXPECT_EQ(indexer.event_batches.front().first, 1);
}

TEST_F(EventRpcTest, CommitPublishesWithoutIndexerDeliveryUntilConfirmed)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  CountingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = {}};
  ASSERT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);

  ASSERT_EQ(event_rpc.eventsched2_open_block(841)(), RPCError::kRPCNone);
  EventBlockResult closed =
      event_rpc.eventsched2_close_block(841, 1, ProposalTxs{})();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(841, block_one)(),
            RPCError::kRPCNone);
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(1).value_or(false));
  EXPECT_EQ(indexer.events.load(std::memory_order_relaxed), 0UL);

  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (indexer.events.load(std::memory_order_relaxed) != 1) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

TEST_F(EventRpcTest, ExactConfirmationIsIdempotentAndEnqueuesOnce)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  CountingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = {}};
  ASSERT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);

  ASSERT_EQ(event_rpc.eventsched2_open_block(842)(), RPCError::kRPCNone);
  EventBlockResult closed =
      event_rpc.eventsched2_close_block(842, 1, ProposalTxs{})();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(842, block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (indexer.events.load(std::memory_order_relaxed) != 1) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(indexer.events.load(std::memory_order_relaxed), 1UL);
}

TEST_F(EventRpcTest, ConfirmationRejectsConflictingAndOutOfOrderValues)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  CountingIndexerRpc indexer;
  IndexerCardV2 card{.protocol = version::kHogdProtocol(),
                     .generation = {},
                     .address_for_event = {},
                     .progress = {}};
  ASSERT_EQ(ctx.register_indexer(make_socketpair_indexer_client(&indexer),
                                 card)(),
            EventIndexerRegistrationOutcome::EventRegistrationAccepted);

  ASSERT_EQ(event_rpc.eventsched2_open_block(843)(), RPCError::kRPCNone);
  EventBlockResult first_closed =
      event_rpc.eventsched2_close_block(843, 1, ProposalTxs{})();
  ASSERT_EQ(first_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, first_closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(843, block_one)(),
            RPCError::kRPCNone);

  ASSERT_EQ(event_rpc.eventsched2_open_block(844)(), RPCError::kRPCNone);
  EventBlockResult second_closed =
      event_rpc.eventsched2_close_block(844, 2, ProposalTxs{})();
  ASSERT_EQ(second_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_two =
      make_consensus_value(2, second_closed.root_hash(), ProposalTxs{});
  block_two.prev_hash = sha256(block_one);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(844, block_two)(),
            RPCError::kRPCNone);

  ConsensusValue conflicting = block_one;
  conflicting.prev_hash[0] ^= 1;
  EXPECT_EQ(event_rpc.eventsched2_confirm_committed_block(conflicting)(),
            RPCError::kRPCBadBlockID);
  EXPECT_EQ(event_rpc.eventsched2_confirm_committed_block(block_two)(),
            RPCError::kRPCUnrecoverable);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  EXPECT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_two)(),
            RPCError::kRPCNone);
  EXPECT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCUnrecoverable);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (indexer.events.load(std::memory_order_relaxed) != 2) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

TEST_F(EventRpcTest, TxFailureReportRelaysToAllIndexers)
{
  EventRPCContext ctx(temp_dir());

  FailureCapturingIndexerRpc indexer_a;
  FailureCapturingIndexerRpc indexer_b;
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_a));
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_b));

  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x5A);
  TxFailureDiagnostic diag;
  diag.attempt_height = 42;
  diag.conflict.code = DbConflictCode::kDbConflictReplayProtection;

  ctx.report_tx_failure_to_indexers(tx_hash, diag)();

  for (const auto *indexer : {&indexer_a, &indexer_b}) {
    ASSERT_EQ(indexer->reports.size(), 1);
    EXPECT_EQ(indexer->reports[0].first, tx_hash);
    EXPECT_EQ(indexer->reports[0].second.attempt_height, 42);
    EXPECT_EQ(indexer->reports[0].second.conflict.code,
              DbConflictCode::kDbConflictReplayProtection);
  }
}

TEST_F(EventRpcTest, TxFailureReportWithNoIndexersIsNoop)
{
  EventRPCContext ctx(temp_dir());
  const SignedTransactionHash tx_hash{};
  const TxFailureDiagnostic diag{};
  // Must complete without error or hang.
  ctx.report_tx_failure_to_indexers(tx_hash, diag)();
}

TEST_F(EventRpcTest, DeadIndexerDoesNotBlockRelayToOthers)
{
  EventRPCContext ctx(temp_dir());

  // First "indexer": a client whose peer is closed immediately, so the send
  // to it fails at the RPC layer.
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  CHECK_EQ(close(fds[1]), 0);
  auto dead_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  ctx.add_indexer(dead_stream->client<IndexerEvent2>());

  FailureCapturingIndexerRpc live_indexer;
  ctx.add_indexer(make_socketpair_indexer_client(&live_indexer));

  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x11);
  TxFailureDiagnostic diag;
  diag.attempt_height = 7;
  diag.conflict.code = DbConflictCode::kDbConflictExclusiveLock;

  // Must complete (dead indexer's error swallowed) and still reach the
  // live indexer.
  ctx.report_tx_failure_to_indexers(tx_hash, diag)();

  ASSERT_EQ(live_indexer.reports.size(), 1);
  EXPECT_EQ(live_indexer.reports[0].first, tx_hash);
  EXPECT_EQ(live_indexer.reports[0].second.attempt_height, 7);
}

// Regression for #3124 (and its review): the fire-and-forget tx-failure relay
// path (eventexecutor_report_tx_failure -> spawn_tx_failure_relay) has no
// natural backpressure -- each relay registers one pending RPC XID per indexer
// that clears only when the indexer replies or its stream fails. A contract
// can drive an unbounded number of these reports, so a configured indexer that
// stays connected but withholds replies would otherwise let that per-report
// RPC state accumulate without bound. relay_tx_failure caps in-flight relays
// PER INDEXER: the unresponsive indexer is bounded at its own cap, while a
// healthy indexer keeps receiving every report (no cross-indexer starvation).
TEST_F(EventRpcTest, TxFailureRelayCapIsPerIndexer)
{
  EventRPCContext ctx(temp_dir());
  constexpr size_t kCap =
      EventRPCContext::kMaxInFlightTxFailureRelaysPerIndexer;

  // Stuck indexer: drains records but never replies, so its relays stay
  // pending and never free their per-indexer slots. The peer counts the
  // records that actually reach its wire.
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  std::atomic<size_t> stuck_records = 0;
  std::jthread peer(
      [fd = fds[1], &stuck_records] { count_rpc_records(fd, stuck_records); });
  auto stuck_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  ctx.add_indexer(stuck_stream->client<IndexerEvent2>());

  // Healthy indexer: replies immediately.
  CountingIndexerRpc healthy;
  ctx.add_indexer(make_socketpair_indexer_client(&healthy));

  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x23);
  TxFailureDiagnostic diag;
  diag.attempt_height = 2522;
  diag.conflict.code = DbConflictCode::kDbConflictReplayProtection;

  // Fire one report at a time, waiting for the healthy indexer to receive each
  // before firing the next. Pacing keeps the healthy indexer's own in-flight
  // count near zero (so it never hits the cap), while the stuck indexer's
  // count only climbs -- isolating the two. Fire well past the cap.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  constexpr size_t kReports = kCap + 16;
  for (size_t i = 0; i < kReports; ++i) {
    tx_hash[0] = static_cast<uint8_t>(i);
    ctx.spawn_tx_failure_relay(tx_hash, diag);
    const size_t want = i + 1;
    while (healthy.reports.load(std::memory_order_relaxed) < want) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline)
          << "healthy indexer received "
          << healthy.reports.load(std::memory_order_relaxed) << " of " << want;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // The healthy indexer received every report -- never starved by the stuck
  // one -- and the stuck indexer is bounded at its own per-indexer cap.
  EXPECT_EQ(healthy.reports.load(std::memory_order_relaxed), kReports);
  while (stuck_records.load(std::memory_order_acquire) < kCap) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline)
        << "stuck indexer received "
        << stuck_records.load(std::memory_order_acquire) << " of " << kCap;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(stuck_records.load(std::memory_order_acquire), kCap);

  // Teardown: failing the stuck stream completes its pending relays (releasing
  // the indexer snapshot), and the shutdown wakes the peer thread so its
  // jthread join cannot deadlock on a socket the client side would only close
  // once ctx is destroyed.
  stuck_stream->fail();
  ::shutdown(fds[1], SHUT_RDWR);
}

TEST_F(EventRpcTest, UnresponsiveIndexerDoesNotBlockCommitOrConfirmation)
{
  auto ctx = std::make_unique<EventRPCContext>(temp_dir());
  EventExecRpc exec_rpc(*ctx);
  EventMempoolRpc mempool_rpc(*ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx->reconcile_outbox_tip(0, genesis)().has_value());

  // The peer remains connected but never reads or replies. Strict delivery may
  // remain pending, but consensus commit must not wait for it.
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  auto dead_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  ctx->add_indexer(dead_stream->client<IndexerEvent2>());

  ProposedTransaction body = make_body(7);
  TxEventLog event = make_event(body, 0x07);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(7)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(7)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(7, 1, event, body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(7)(), RPCError::kRPCNone);
  EventBlockResult close_result =
      mempool_rpc.eventsched2_close_block(7, 1, proposal_txs)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);

  // Commit must succeed without waiting for the dead indexer's delivery RPC.
  ConsensusValue value =
      make_consensus_value(1, close_result.root_hash(), proposal_txs);
  value.prev_hash = sha256(genesis);
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(7, value)(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched2_confirm_committed_block(value)(),
            RPCError::kRPCNone);

  // Commit releases the block id, but removal of the context from the map
  // ("last reference out does the GC") may be deferred to close_block's
  // detached file-write coroutine, so an immediate reopen can transiently
  // see kExists. Retry with a deadline; any other error is a real failure.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (true) {
    auto reopen = ctx->open_block(7)();
    if (reopen.has_value()) {
      break;
    }
    ASSERT_EQ(reopen.error().code(), EventErrorCode::kExists);
    ASSERT_LT(std::chrono::steady_clock::now(), deadline)
        << "block id 7 still not reusable long after commit";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(7)(), RPCError::kRPCNone);
  auto shutdown = std::async(std::launch::async, [&ctx] { ctx.reset(); });
  ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  shutdown.get();
  CHECK_EQ(close(fds[1]), 0);
}

TEST_F(EventRpcTest, ContextShutdownCancelsPendingDeliveryAndDrainsWorker)
{
  auto ctx = std::make_unique<EventRPCContext>(temp_dir());
  EventMempoolRpc event_rpc(*ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx->reconcile_outbox_tip(0, genesis)().has_value());

  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  std::atomic<size_t> records_read = 0;
  std::jthread peer(
      [fd = fds[1], &records_read] { count_rpc_records(fd, records_read); });
  auto stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  ctx->add_indexer(stream->client<IndexerEvent2>());

  ASSERT_EQ(event_rpc.eventsched2_open_block(85)(), RPCError::kRPCNone);
  EventBlockResult closed =
      event_rpc.eventsched2_close_block(85, 1, ProposalTxs{})();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(85, block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (records_read.load(std::memory_order_acquire) == 0) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto shutdown = std::async(std::launch::async, [&ctx] { ctx.reset(); });
  ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  shutdown.get();
}

TEST_F(EventRpcTest, ContextShutdownToleratesSuccessfulDeliveryReply)
{
  auto ctx = std::make_unique<EventRPCContext>(temp_dir());
  EventMempoolRpc event_rpc(*ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx->reconcile_outbox_tip(0, genesis)().has_value());
  BlockingIndexerRpc indexer;
  ctx->add_indexer(make_socketpair_indexer_client(&indexer));

  std::latch delivery_succeeded{1};
  std::latch release_delivery{1};
  ctx->set_delivery_succeeded_test_hook([&] {
    delivery_succeeded.count_down();
    release_delivery.wait();
  });

  ASSERT_EQ(event_rpc.eventsched2_open_block(86)(), RPCError::kRPCNone);
  EventBlockResult closed =
      event_rpc.eventsched2_close_block(86, 1, ProposalTxs{})();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched2_commit_block(86, block_one)(),
            RPCError::kRPCNone);
  ASSERT_EQ(event_rpc.eventsched2_confirm_committed_block(block_one)(),
            RPCError::kRPCNone);
  indexer.apply_started.wait();
  indexer.release_apply.count_down();
  delivery_succeeded.wait();

  auto shutdown = std::async(std::launch::async, [&ctx] { ctx.reset(); });
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);
  release_delivery.count_down();
  ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  shutdown.get();
}

TEST_F(EventRpcTest, DuplicateOpenReturnsExists)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(3)(), RPCError::kRPCNone);

  // A second open of a live block id must fail cleanly with kExists (not
  // abort constructing-then-destroying an active context).
  auto status = ctx.open_block(3)();
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), EventErrorCode::kExists);

  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(3)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, RemovedIndexerStopsReceivingReports)
{
  EventRPCContext ctx(temp_dir());

  FailureCapturingIndexerRpc indexer_a;
  FailureCapturingIndexerRpc indexer_b;
  uint64_t id_a = ctx.add_indexer(make_socketpair_indexer_client(&indexer_a));
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_b));

  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x21);
  TxFailureDiagnostic diag;
  diag.attempt_height = 3;
  diag.conflict.code = DbConflictCode::kDbConflictReplayProtection;

  ctx.report_tx_failure_to_indexers(tx_hash, diag)();
  ASSERT_EQ(indexer_a.reports.size(), 1);
  ASSERT_EQ(indexer_b.reports.size(), 1);

  EXPECT_TRUE(ctx.remove_indexer(id_a));
  // Removing an id that is already gone reports failure.
  EXPECT_FALSE(ctx.remove_indexer(id_a));

  ctx.report_tx_failure_to_indexers(tx_hash, diag)();
  EXPECT_EQ(indexer_a.reports.size(), 1);
  ASSERT_EQ(indexer_b.reports.size(), 2);
}

TEST_F(EventRpcTest, RemoveLastIndexerMakesReportsNoop)
{
  EventRPCContext ctx(temp_dir());

  FailureCapturingIndexerRpc indexer;
  uint64_t id = ctx.add_indexer(make_socketpair_indexer_client(&indexer));
  EXPECT_TRUE(ctx.remove_indexer(id));

  const SignedTransactionHash tx_hash{};
  const TxFailureDiagnostic diag{};
  // Must complete without error or hang.
  ctx.report_tx_failure_to_indexers(tx_hash, diag)();
  EXPECT_TRUE(indexer.reports.empty());
}

TEST_F(EventRpcTest, BasicUsage)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_NE(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  ProposedTransaction body = make_body(1);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 1),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(2, 2, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);

  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result =
      mempool_rpc.eventsched2_close_block(1, 1, make_proposal_txs({body}))();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestAbandonBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);

  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestEmptyBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  ProposalTxs proposal_txs;
  EventBlockResult close_result =
      mempool_rpc.eventsched2_close_block(1, 1, proposal_txs)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(1, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(1, value)(),
            RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestFromRPC)
{
  EventRPCContext ctx(temp_dir());
  RpcServer<EventExecRpc> exec_rpc(ctx);
  RpcServer<EventMempoolRpc> mempool_rpc(ctx);

  auto exec_port = exec_rpc.start_server(0);
  auto mempool_port = mempool_rpc.start_server(0);

  auto exec_addr = host_port_to_sockaddr("localhost", exec_port);
  auto mempool_addr = host_port_to_sockaddr("localhost", mempool_port);

  auto exec_client = x23::SRpcMulticlient<x23::RpcLogger>::make(exec_addr)
                         ->client<EventExecutor1>();
  auto mempool_client = x23::SRpcMulticlient<x23::RpcLogger>::make(mempool_addr)
                            ->client<EventSched2>();
  EXPECT_NE(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);

  EXPECT_NE(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{},
                                                 ProposedTransaction{}),
            RPCError::kRPCNone);

  EXPECT_EQ(mempool_client.eventsched2_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);

  ProposedTransaction body = make_body(10);
  EXPECT_EQ(exec_client.eventexecutor_emit_event(0, 0, make_event(body, 10),
                                                 body),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  EventBlockResult close_result =
      mempool_client.eventsched2_close_block(0, 0, proposal_txs);
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(0, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_client.eventsched2_commit_block(0, value),
            RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestFromRPCWithExplicitIpv4Bind)
{
  EventRPCContext ctx(temp_dir());
  RpcServer<EventExecRpc> exec_rpc(ctx);
  RpcServer<EventMempoolRpc> mempool_rpc(ctx);

  auto exec_port = exec_rpc.start_server(x23::HostPort{"127.0.0.1", 0});
  auto mempool_port = mempool_rpc.start_server(x23::HostPort{"127.0.0.1", 0});

  auto exec_addr = host_port_to_sockaddr("127.0.0.1", exec_port);
  auto mempool_addr = host_port_to_sockaddr("127.0.0.1", mempool_port);

  auto exec_client = x23::SRpcMulticlient<x23::RpcLogger>::make(exec_addr)
                         ->client<EventExecutor1>();
  auto mempool_client = x23::SRpcMulticlient<x23::RpcLogger>::make(mempool_addr)
                            ->client<EventSched2>();

  EXPECT_EQ(mempool_client.eventsched2_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  ProposalTxs proposal_txs;
  EventBlockResult close_result =
      mempool_client.eventsched2_close_block(0, 0, proposal_txs);
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(0, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_client.eventsched2_commit_block(0, value),
            RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestFromRPCWithExplicitIpv6Bind)
{
  if (!x23::HostPort{"::1", 0}.to_addr().has_value()) {
    GTEST_SKIP() << "IPv6 loopback is unavailable";
  }

  EventRPCContext ctx(temp_dir());
  RpcServer<EventExecRpc> exec_rpc(ctx);
  RpcServer<EventMempoolRpc> mempool_rpc(ctx);

  auto exec_port = exec_rpc.start_server(x23::HostPort{"::1", 0});
  auto mempool_port = mempool_rpc.start_server(x23::HostPort{"::1", 0});

  auto exec_addr = host_port_to_sockaddr("::1", exec_port);
  auto mempool_addr = host_port_to_sockaddr("::1", mempool_port);

  auto exec_client = x23::SRpcMulticlient<x23::RpcLogger>::make(exec_addr)
                         ->client<EventExecutor1>();
  auto mempool_client = x23::SRpcMulticlient<x23::RpcLogger>::make(mempool_addr)
                            ->client<EventSched2>();

  EXPECT_EQ(mempool_client.eventsched2_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  ProposalTxs proposal_txs;
  EventBlockResult close_result =
      mempool_client.eventsched2_close_block(0, 0, proposal_txs);
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(0, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_client.eventsched2_commit_block(0, value),
            RPCError::kRPCNone);
}

// Test multiple executors with proper reference counting
TEST_F(EventRpcTest, MultipleExecutors)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);

  // Three executors open handles
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Add events from different executors
  ProposedTransaction body_a = make_body(11);
  ProposedTransaction body_b = make_body(12);
  ProposalTxs proposal_txs = make_proposal_txs({body_a, body_b});
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body_a, 11),
                                              body_a)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 2, make_event(body_b, 12),
                                              body_b)(),
            RPCError::kRPCNone);

  // Close two executors
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  // close_block should wait for the third executor
  std::atomic<bool> close_started = false;
  std::atomic<bool> close_completed = false;
  std::latch close_reserved(1);
  ctx.set_close_reserved_test_hook(
      [&](BlockID /*block_id*/) { close_reserved.count_down(); });
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    close_started.store(true, std::memory_order_release);
    auto result =
        co_await mempool_rpc.eventsched2_close_block(1, 1, proposal_txs);
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    close_completed.store(true, std::memory_order_release);
    co_return;
  }();

  close_reserved.wait();
  EXPECT_TRUE(close_started.load(std::memory_order_acquire));
  EXPECT_FALSE(close_completed.load(std::memory_order_acquire));

  // close_block should be waiting for the third executor
  // (This is hard to verify deterministically, but we can at least check it
  // eventually completes)

  // Close the third executor
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  async();
  EXPECT_TRUE(close_completed.load(std::memory_order_acquire));
}

// Test that close_block waits for executor handles even when called first
TEST_F(EventRpcTest, CloseBlockWaitsForExecutors)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Start close_block in background (it should wait for executor to close)
  ProposedTransaction body = make_body(13);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  std::atomic<bool> close_completed = false;
  std::latch close_reserved(1);
  ctx.set_close_reserved_test_hook(
      [&](BlockID /*block_id*/) { close_reserved.count_down(); });
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    auto result =
        co_await mempool_rpc.eventsched2_close_block(1, 1, proposal_txs);
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    close_completed.store(true, std::memory_order_release);
    co_return;
  }();

  close_reserved.wait();

  // close_block should not have completed yet
  EXPECT_FALSE(close_completed.load(std::memory_order_acquire));

  // Add an event and close executor
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 13),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  // Now close_block should complete
  async();
  EXPECT_TRUE(close_completed.load(std::memory_order_acquire));
}

TEST_F(EventRpcTest, TestAbandonBlockThenCloseBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(), RPCError::kRPCNone);
  // should return an error because the block is abandoned
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCBadBlockID);
}

// Test abandoning a block while close_block is waiting for executors
TEST_F(EventRpcTest, AbandonIsRejectedWhileCloseBlockIsReserved)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Start close_block in background (will wait for executor)
  std::atomic<bool> close_completed = false;
  std::latch close_reserved(1);
  ctx.set_close_reserved_test_hook(
      [&](BlockID /*block_id*/) { close_reserved.count_down(); });
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    auto result =
        co_await mempool_rpc.eventsched2_close_block(1, 1, ProposalTxs{});
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    close_completed.store(true);
  }();

  close_reserved.wait();

  // The close reservation prevents abandon from destructively racing it.
  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(),
            RPCError::kRPCBadBlockID);

  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  // close_block completes after its executor reference drains.
  async();
  EXPECT_TRUE(close_completed.load());
}

TEST_F(EventRpcTest, CommitIsRejectedWhileCloseBlockIsReserved)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(2)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(2)(), RPCError::kRPCNone);

  std::latch close_reserved(1);
  ctx.set_close_reserved_test_hook(
      [&](BlockID /*block_id*/) { close_reserved.count_down(); });
  auto close_async = mempool_rpc.eventsched2_close_block(2, 2, proposal_txs);

  close_reserved.wait();
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(
                2, make_consensus_value(2, {}, proposal_txs))(),
            RPCError::kRPCBadBlockID);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(2)(), RPCError::kRPCNone);
  EventBlockResult close_result = close_async();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(2, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(2, value)(),
            RPCError::kRPCNone);
}

// Regression for #3039: closing a block that was abandoned while a context
// reference was still outstanding must fail with kBadBlockID. The abandon
// leaves the context in the map in ARC-shutdown mode because the live reference
// prevents destruction. Close must reject the shutdown context without
// releasing a reference it never acquired.
TEST_F(EventRpcTest, CloseAfterAbandonWithOutstandingReferenceIsRejected)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Models an in-flight executor RPC holding the block's context.
  auto held_ref = ctx.get_context(1)();
  ASSERT_TRUE(held_ref);

  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(), RPCError::kRPCNone);

  EXPECT_EQ(mempool_rpc.eventsched2_close_block(1, 1, ProposalTxs{})().error(),
            RPCError::kRPCBadBlockID);

  // The held reference is the last owner; release-then-gc, as the executor
  // RPC handlers do.
  if (held_ref.release_reference()) {
    ctx.gc_context(1)();
  }
}

// Companion to CloseAfterAbandonWithOutstandingReferenceIsRejected (#3039):
// close and abandon compete for one per-block operation reservation. This
// hammers the interleaving with a swept timing offset; one operation may win,
// but every outcome must be a clean error code.
TEST_F(EventRpcTest, AbandonRacingCloseAfterExecutorsClosedIsClean)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  using namespace std::chrono_literals;
  for (BlockID block_id = 1; block_id <= 200; ++block_id) {
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(block_id)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_open_block(block_id)(),
              RPCError::kRPCNone);

    // Keeps the context pinned in the map (shutdown-pending) if the abandon
    // wins the race, modeling an in-flight executor RPC.
    auto held_ref = ctx.get_context(block_id)();
    ASSERT_TRUE(held_ref);

    std::atomic<RPCError> close_err = RPCError::kRPCNone;
    std::latch close_reserved(1);
    ctx.set_close_reserved_test_hook(
        [&](BlockID /*reserved_id*/) { close_reserved.count_down(); });
    auto close_task = [&](this auto) -> x23::Async<void> {
      co_await default_threadpool().bg();
      auto result = co_await mempool_rpc.eventsched2_close_block(block_id, 1,
                                                                 ProposalTxs{});
      close_err.store(result.error(), std::memory_order_release);
      co_return;
    }();

    // Let close_block reach its executor wait, then release the last
    // executor handle (waking it) and immediately race an abandon against
    // its map-lock re-acquisition, sweeping the offset across iterations.
    close_reserved.wait();
    ASSERT_EQ(exec_rpc.eventexecutor_close_block(block_id)(),
              RPCError::kRPCNone);
    std::this_thread::sleep_for( // NOLINT(misc-include-cleaner)
        std::chrono::microseconds(block_id % 16));
    RPCError abandon_err = mempool_rpc.eventsched2_abandon_block(block_id)();

    close_task();
    RPCError observed_close = close_err.load(std::memory_order_acquire);

    // Depending on who wins, either RPC may succeed or lose the block id;
    // anything else (in particular a crash) is the bug.
    EXPECT_TRUE(observed_close == RPCError::kRPCNone ||
                observed_close == RPCError::kRPCBadBlockID)
        << "close error " << static_cast<int>(observed_close) << " at block "
        << block_id;
    EXPECT_TRUE(abandon_err == RPCError::kRPCNone ||
                abandon_err == RPCError::kRPCBadBlockID)
        << "abandon error " << static_cast<int>(abandon_err) << " at block "
        << block_id;

    if (held_ref.release_reference()) {
      ctx.gc_context(block_id)();
    }
  }
}

// Test that processing multiple blocks recycles EventBlockContext internals.
// The second block uses the cached trie/buffers from the first.
TEST_F(EventRpcTest, RecycleContextAcrossBlocks)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  ConsensusValue previous;
  for (BlockID block_id = 1; block_id <= 3; block_id++) {
    EXPECT_EQ(mempool_rpc.eventsched2_open_block(block_id)(),
              RPCError::kRPCNone);
    EXPECT_EQ(exec_rpc.eventexecutor_open_block(block_id)(),
              RPCError::kRPCNone);

    ProposedTransaction body = make_body(block_id + 20);
    ProposalTxs proposal_txs = make_proposal_txs({body});
    EXPECT_EQ(exec_rpc.eventexecutor_emit_event(
                  block_id, 1, make_event(body, static_cast<uint8_t>(block_id)),
                  body)(),
              RPCError::kRPCNone);

    EXPECT_EQ(exec_rpc.eventexecutor_close_block(block_id)(),
              RPCError::kRPCNone);

    auto result =
        mempool_rpc.eventsched2_close_block(block_id, block_id, proposal_txs)();
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    ConsensusValue value =
        make_consensus_value(block_id, result.root_hash(), proposal_txs);
    if (block_id > 1) {
      value.prev_hash = sha256(previous);
    }
    else {
      initialize_test_origin(ctx, value);
    }
    EXPECT_EQ(mempool_rpc.eventsched2_commit_block(block_id, value)(),
              RPCError::kRPCNone);
    previous = std::move(value);
  }
}

TEST_F(EventRpcTest, AbandonActiveBlockDeletesTempFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);

  auto temp_file = EventBlockContext::temp_events_filename(temp_dir(), 1);
  EXPECT_TRUE(std::filesystem::exists(temp_file));

  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_FALSE(std::filesystem::exists(temp_file));
}

TEST_F(EventRpcTest, AbandonClosedBlockPreservesFinalFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  ProposedTransaction body = make_body(31);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 31),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched2_close_block(1, 1, proposal_txs)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  auto final_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());
  auto temp_file = EventBlockContext::temp_events_filename(temp_dir(), 1);

  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_TRUE(std::filesystem::exists(final_file));
  EXPECT_FALSE(std::filesystem::exists(temp_file));
}

TEST_F(EventRpcTest, CommitBlockKeepsFinalFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched2_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  ProposedTransaction body = make_body(32);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 32),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched2_close_block(1, 1, proposal_txs)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  auto final_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());

  ConsensusValue value =
      make_consensus_value(1, result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(1, value)(),
            RPCError::kRPCNone);
  EXPECT_TRUE(std::filesystem::exists(final_file));
}

TEST_F(EventRpcTest, ClosePublishesCandidateAndCommitPromotesIt)
{
  ProposedTransaction body = make_body(41);
  TxEventLog event = make_event(body, 41);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  ConsensusValue consensus_value;

  {
    EventRPCContext ctx(temp_dir());
    EventExecRpc exec_rpc(ctx);
    EventMempoolRpc mempool_rpc(ctx);

    ASSERT_EQ(mempool_rpc.eventsched2_open_block(41)(), RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_open_block(41)(), RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_emit_event(41, 1, event, body)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_close_block(41)(), RPCError::kRPCNone);

    EventBlockResult close_result =
        mempool_rpc.eventsched2_close_block(41, 9, proposal_txs)();
    ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
    auto candidate = ctx.indexer_outbox_store().load_candidate(41);
    ASSERT_TRUE(candidate.has_value()) << candidate.error();
    EXPECT_EQ(candidate->block_id, 41);
    EXPECT_EQ(candidate->block_height, 9);
    EXPECT_EQ(candidate->event_root, close_result.root_hash());
    ASSERT_EQ(candidate->event_logs.size(), 1);
    ASSERT_EQ(candidate->tx_bodies.size(), 1);
    expect_xdr_equal(candidate->proposal_txs, proposal_txs);
    expect_xdr_equal(candidate->event_logs[0], event);
    expect_xdr_equal(candidate->tx_bodies[0], body);

    consensus_value =
        make_consensus_value(9, close_result.root_hash(), proposal_txs);
    consensus_value.proposal_header.block_timepoint = 12345;
    initialize_test_origin(ctx, consensus_value);
    ASSERT_EQ(mempool_rpc.eventsched2_commit_block(41, consensus_value)(),
              RPCError::kRPCNone);

    EXPECT_FALSE(ctx.indexer_outbox_store().load_candidate(41).has_value());
    auto committed = ctx.indexer_outbox_store().load_committed(9);
    ASSERT_TRUE(committed.has_value()) << committed.error();
    expect_xdr_equal(committed->consensus_value, consensus_value);
    expect_xdr_equal(committed->proposal_txs, proposal_txs);
    expect_xdr_equal(committed->event_logs[0], event);
    expect_xdr_equal(committed->tx_bodies[0], body);
  }

  auto reopened = IndexerOutboxStore::open(temp_dir() + "/indexer-outbox");
  ASSERT_TRUE(reopened.has_value()) << reopened.error();
  auto committed = reopened->load_committed(9);
  ASSERT_TRUE(committed.has_value()) << committed.error();
  expect_xdr_equal(committed->consensus_value, consensus_value);
}

TEST_F(EventRpcTest, AbandonClosedBlockRetainsCandidateForReconciliation)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(42);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(42)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(42)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(42, 1, make_event(body, 42),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(42)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_close_block(42, 10, proposal_txs)().error(),
            RPCError::kRPCNone);
  ASSERT_TRUE(ctx.indexer_outbox_store().load_candidate(42).has_value());

  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(42)(), RPCError::kRPCNone);
  EXPECT_TRUE(ctx.indexer_outbox_store().load_candidate(42).has_value());
  EXPECT_FALSE(ctx.indexer_outbox_store().load_committed(10).has_value());
}

TEST_F(EventRpcTest, CommitRemovesPreviouslyAbandonedCandidateAtSameHeight)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(50)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(51)(), RPCError::kRPCNone);
  EventBlockResult selected =
      mempool_rpc.eventsched2_close_block(50, 10, proposal_txs)();
  EventBlockResult abandoned =
      mempool_rpc.eventsched2_close_block(51, 10, proposal_txs)();
  ASSERT_EQ(selected.error(), RPCError::kRPCNone);
  ASSERT_EQ(abandoned.error(), RPCError::kRPCNone);
  ASSERT_TRUE(ctx.indexer_outbox_store().load_candidate(50).has_value());
  ASSERT_TRUE(ctx.indexer_outbox_store().load_candidate(51).has_value());

  // Models the old candidate leaving mempool's commit cache. There is
  // deliberately no second abandon after the competing candidate commits.
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(51)(), RPCError::kRPCNone);
  EXPECT_FALSE(ctx.get_context(51)());
  ASSERT_TRUE(ctx.indexer_outbox_store().has_candidate(51).value());

  ConsensusValue value =
      make_consensus_value(10, selected.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(50, value)(),
            RPCError::kRPCNone);

  EXPECT_FALSE(ctx.indexer_outbox_store().has_candidate(51).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().load_committed(10).has_value());
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir(),
                                               abandoned.root_hash())));
}

TEST_F(EventRpcTest,
       CommitRemovesArchivalCandidateByHeaderAndPreservesFutureCandidate)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction archival_body = make_body(52);
  ProposedTransaction future_body = make_body(53);
  ProposedTransaction selected_body = make_body(54);
  ProposalTxs archival_txs = make_proposal_txs({archival_body});
  ProposalTxs future_txs = make_proposal_txs({future_body});
  ProposalTxs selected_txs = make_proposal_txs({selected_body});

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(52)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(53)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(54)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(52)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(53)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(54)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(52, 1,
                                              make_event(archival_body, 52),
                                              archival_body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(53, 1,
                                              make_event(future_body, 53),
                                              future_body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(54, 1,
                                              make_event(selected_body, 54),
                                              selected_body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(52)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(53)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(54)(), RPCError::kRPCNone);
  EventBlockResult archival =
      mempool_rpc.eventsched2_close_block(52, 9, archival_txs)();
  EventBlockResult future =
      mempool_rpc.eventsched2_close_block(53, 11, future_txs)();
  EventBlockResult selected =
      mempool_rpc.eventsched2_close_block(54, 10, selected_txs)();
  ASSERT_EQ(archival.error(), RPCError::kRPCNone);
  ASSERT_EQ(future.error(), RPCError::kRPCNone);
  ASSERT_EQ(selected.error(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(52)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(53)(), RPCError::kRPCNone);

  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::candidate_filename(52),
                          "hogp/archival");
  replace_outbox_payload(temp_dir(), IndexerOutboxStore::candidate_filename(52),
                         x23::Buf{0xff, 0xff, 0xff, 0xff});
  const std::filesystem::path archival_events =
      EventBlockContext::final_events_filename(temp_dir(),
                                               archival.root_hash());
  const std::filesystem::path future_events =
      EventBlockContext::final_events_filename(temp_dir(), future.root_hash());

  ConsensusValue value =
      make_consensus_value(10, selected.root_hash(), selected_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(54, value)(),
            RPCError::kRPCNone);

  EXPECT_FALSE(ctx.indexer_outbox_store().has_candidate(52).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(53).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(10).value());
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  EXPECT_TRUE(std::filesystem::exists(archival_events));
  EXPECT_TRUE(std::filesystem::exists(future_events));
}

TEST_F(EventRpcTest, CommitCleanupFailsClosedOnCorruptCurrentCandidate)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(55)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(56)(), RPCError::kRPCNone);
  EventBlockResult stale =
      mempool_rpc.eventsched2_close_block(55, 10, proposal_txs)();
  EventBlockResult selected =
      mempool_rpc.eventsched2_close_block(56, 10, proposal_txs)();
  ASSERT_EQ(stale.error(), RPCError::kRPCNone);
  ASSERT_EQ(selected.error(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(55)(), RPCError::kRPCNone);
  corrupt_outbox_payload(temp_dir(),
                         IndexerOutboxStore::candidate_filename(55));
  ConsensusValue value =
      make_consensus_value(10, selected.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);

  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(56, value)(),
            RPCError::kRPCUnrecoverable);
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(55).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(56).value());
  EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(10).value());

  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::candidate_filename(55),
                          "hogp/archival");
  replace_outbox_payload(temp_dir(), IndexerOutboxStore::candidate_filename(55),
                         x23::Buf{0xff, 0xff, 0xff, 0xff});
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(56, value)(),
            RPCError::kRPCNone);
  EXPECT_FALSE(ctx.indexer_outbox_store().has_candidate(55).value());
  EXPECT_FALSE(ctx.indexer_outbox_store().has_candidate(56).value());
}

TEST_F(EventRpcTest, EmptyBlockPublishesCandidateAndCommittedRecords)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(43)(), RPCError::kRPCNone);
  EventBlockResult close_result =
      mempool_rpc.eventsched2_close_block(43, 11, proposal_txs)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  auto candidate = ctx.indexer_outbox_store().load_candidate(43);
  ASSERT_TRUE(candidate.has_value()) << candidate.error();
  EXPECT_TRUE(candidate->event_logs.empty());
  EXPECT_TRUE(candidate->tx_bodies.empty());

  ConsensusValue value =
      make_consensus_value(11, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(43, value)(),
            RPCError::kRPCNone);
  auto committed = ctx.indexer_outbox_store().load_committed(11);
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_TRUE(committed->event_logs.empty());
  EXPECT_TRUE(committed->tx_bodies.empty());
  expect_xdr_equal(committed->consensus_value, value);
}

TEST_F(EventRpcTest, CompetingFirstCommitsPublishOneOriginAndRecord)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction first_body = make_body(430);
  ProposedTransaction second_body = make_body(431);
  ProposalTxs first_txs = make_proposal_txs({first_body});
  ProposalTxs second_txs = make_proposal_txs({second_body});

  for (const auto &[block_id, body] :
       std::vector<std::pair<BlockID, ProposedTransaction>>{{430, first_body},
                                                            {431,
                                                             second_body}}) {
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(block_id)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_open_block(block_id)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_emit_event(
                  block_id, 1, make_event(body, static_cast<uint8_t>(block_id)),
                  body)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_close_block(block_id)(),
              RPCError::kRPCNone);
  }
  EventBlockResult first_closed =
      mempool_rpc.eventsched2_close_block(430, 1, first_txs)();
  EventBlockResult second_closed =
      mempool_rpc.eventsched2_close_block(431, 1, second_txs)();
  ASSERT_EQ(first_closed.error(), RPCError::kRPCNone);
  ASSERT_EQ(second_closed.error(), RPCError::kRPCNone);
  ConsensusValue first =
      make_consensus_value(1, first_closed.root_hash(), first_txs);
  ConsensusValue second =
      make_consensus_value(1, second_closed.root_hash(), second_txs);
  ConsensusValueHash parent{};
  parent[0] = 0x43;
  first.prev_hash = parent;
  second.prev_hash = parent;
  initialize_test_origin(ctx, first);

  auto first_commit = mempool_rpc.eventsched2_commit_block(430, first);
  auto second_commit = mempool_rpc.eventsched2_commit_block(431, second);
  const RPCError first_result = first_commit();
  const RPCError second_result = second_commit();
  EXPECT_NE(first_result == RPCError::kRPCNone,
            second_result == RPCError::kRPCNone);

  auto committed = ctx.indexer_outbox_store().load_committed(1);
  ASSERT_TRUE(committed.has_value()) << committed.error();
  auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->origin.first_height, 1);
  EXPECT_EQ(current_origin->origin.first_prev_hash,
            committed->consensus_value.prev_hash);
  if (first_result == RPCError::kRPCNone) {
    expect_xdr_equal(committed->consensus_value, first);
  }
  else {
    expect_xdr_equal(committed->consensus_value, second);
  }
}

TEST_F(EventRpcTest, CommitRejectsMismatchedProposalCommitment)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(44);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(44)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(44)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(44, 1, make_event(body, 44),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(44)(), RPCError::kRPCNone);
  EventBlockResult close_result =
      mempool_rpc.eventsched2_close_block(44, 12, proposal_txs)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);

  ConsensusValue mismatched =
      make_consensus_value(12, close_result.root_hash(), proposal_txs);
  mismatched.proposal_txs_hash[0] ^= 0x01;
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(44, mismatched)(),
            RPCError::kRPCUnrecoverable);
  EXPECT_FALSE(ctx.indexer_outbox_store().load_committed(12).has_value());
  EXPECT_TRUE(ctx.indexer_outbox_store().load_candidate(44).has_value());
  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(44)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, IdenticalCommittedPrepublicationIsIdempotentAcrossBlockIds)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(45)(), RPCError::kRPCNone);
  EventBlockResult close_result =
      mempool_rpc.eventsched2_close_block(45, 13, proposal_txs)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(13, close_result.root_hash(), proposal_txs);
  auto candidate = ctx.indexer_outbox_store().load_candidate(45);
  ASSERT_TRUE(candidate.has_value()) << candidate.error();
  IndexerOutboxCommittedPayload committed{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = value,
      .proposal_txs = candidate->proposal_txs,
      .event_logs = candidate->event_logs,
      .tx_bodies = candidate->tx_bodies,
  };
  initialize_test_origin(ctx, value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(committed).has_value());

  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(45, value)(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(99, value)(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(45, value)(),
            RPCError::kRPCNone);
  EXPECT_FALSE(ctx.indexer_outbox_store().has_candidate(45).value());
  auto loaded = ctx.indexer_outbox_store().load_committed(13);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  expect_xdr_equal(*loaded, committed);
}

TEST_F(EventRpcTest, PrepublishedConsensusRejectsDifferentFullCandidatePayload)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(451);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(451)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(451)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(451, 1, make_event(body, 0x45),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(451)(), RPCError::kRPCNone);
  EventBlockResult closed =
      mempool_rpc.eventsched2_close_block(451, 13, proposal_txs)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(13, closed.root_hash(), proposal_txs);
  auto original = ctx.indexer_outbox_store().load_candidate(451);
  ASSERT_TRUE(original.has_value()) << original.error();
  IndexerOutboxCommittedPayload committed{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = value,
      .proposal_txs = original->proposal_txs,
      .event_logs = original->event_logs,
      .tx_bodies = original->tx_bodies,
  };
  initialize_test_origin(ctx, value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(committed).has_value());
  ASSERT_TRUE(ctx.indexer_outbox_store().remove_candidate(451).has_value());

  IndexerOutboxCandidatePayload changed = *original;
  changed.block_id = 452;
  changed.tx_bodies[0].signed_tx.signatures.push_back(DecoratedSignature{});
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_candidate(changed).has_value());
  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(452, value)(),
            RPCError::kRPCUnrecoverable);
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(452).value());
  auto retained = ctx.indexer_outbox_store().load_committed(13);
  ASSERT_TRUE(retained.has_value()) << retained.error();
  expect_xdr_equal(*retained, committed);
}

TEST_F(EventRpcTest, DuplicateCloseReturnsDurableCandidateWithoutReinitializing)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(46)(), RPCError::kRPCNone);
  EventBlockResult first =
      mempool_rpc.eventsched2_close_block(46, 14, proposal_txs)();
  ASSERT_EQ(first.error(), RPCError::kRPCNone);
  EventBlockResult retry =
      mempool_rpc.eventsched2_close_block(46, 14, proposal_txs)();
  ASSERT_EQ(retry.error(), RPCError::kRPCNone);
  EXPECT_EQ(retry.root_hash(), first.root_hash());

  EXPECT_EQ(mempool_rpc.eventsched2_close_block(46, 15, proposal_txs)().error(),
            RPCError::kRPCBadBlockID);
  EXPECT_EQ(mempool_rpc
                .eventsched2_close_block(46, 14,
                                         make_proposal_txs({make_body(46)}))()
                .error(),
            RPCError::kRPCBadBlockID);
  EXPECT_EQ(mempool_rpc.eventsched2_abandon_block(46)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, CommitPromotesRetainedCandidateAfterContextRestart)
{
  ProposalTxs proposal_txs;
  ConsensusValue value;
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  std::filesystem::path final_file;
  {
    EventRPCContext ctx(temp_dir());
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(47)(), RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(47, 1, proposal_txs)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    final_file = EventBlockContext::final_events_filename(temp_dir(),
                                                          closed.root_hash());
    value = make_consensus_value(1, closed.root_hash(), proposal_txs);
    value.prev_hash = sha256(genesis);
    ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(47)(), RPCError::kRPCNone);
    ASSERT_TRUE(std::filesystem::exists(final_file));
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(47, value)().has_value());
  EXPECT_TRUE(std::filesystem::exists(final_file));
  EXPECT_FALSE(restarted.indexer_outbox_store().has_candidate(47).value());
  auto committed = restarted.indexer_outbox_store().load_committed(1);
  ASSERT_TRUE(committed.has_value()) << committed.error();
  expect_xdr_equal(committed->consensus_value, value);
}

TEST_F(EventRpcTest, ReconciliationFailsWithoutCurrentEpochOrigin)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(48)(), RPCError::kRPCNone);
  EventBlockResult closed =
      mempool_rpc.eventsched2_close_block(48, 16, proposal_txs)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(16, closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(48, value)(),
            RPCError::kRPCNone);
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));

  EXPECT_FALSE(ctx.reconcile_outbox_tip(48, value)().has_value());
}

TEST_F(EventRpcTest, ReconciliationInitializesOriginOnlyAtEmptyGenesis)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    ASSERT_TRUE(origin->has_value());
    const auto &origin_value = origin.value();
    const auto *current_origin =
        origin_value.has_value() ? &origin_value.value() : nullptr;
    ASSERT_NE(current_origin, nullptr);
    EXPECT_EQ(current_origin->origin.first_height, 1);
    EXPECT_EQ(current_origin->origin.first_prev_hash, sha256(genesis));
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
  auto origin = restarted.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->origin.first_height, 1);
  EXPECT_EQ(current_origin->origin.first_prev_hash, sha256(genesis));
}

TEST_F(EventRpcTest, EmptyChainReconciliationDoesNotInferOrigin)
{
  ConsensusValue no_blocks{};
  no_blocks.proposal_header.block_height = -1;
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(ctx.reconcile_outbox_tip(std::numeric_limits<BlockID>::max(),
                                         no_blocks)()
                    .has_value());
    auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    EXPECT_FALSE(origin->has_value());
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted
                  .reconcile_outbox_tip(std::numeric_limits<BlockID>::max(),
                                        no_blocks)()
                  .has_value());
  auto origin = restarted.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  EXPECT_FALSE(origin->has_value());
}

TEST_F(EventRpcTest,
       PreReconciliationCommitAndPromotionCannotDeriveCurrentOrigin)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;

  constexpr BlockID kHeightOneBlock = 780;
  constexpr BlockID kMidchainBlock = 781;
  ConsensusValue height_one;
  ConsensusValue midchain;
  for (const auto &[block_id, height] :
       {std::pair{kHeightOneBlock, BlockHeight{1}},
        std::pair{kMidchainBlock, BlockHeight{37}}}) {
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(block_id)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(block_id, height, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    ConsensusValue value =
        make_consensus_value(height, closed.root_hash(), ProposalTxs{});
    if (height == 1) {
      value.prev_hash = sha256(genesis);
      height_one = value;
    }
    else {
      value.prev_hash[0] = 0x37;
      midchain = value;
    }

    EXPECT_EQ(mempool_rpc.eventsched2_commit_block(block_id, value)(),
              RPCError::kRPCUnrecoverable);
    EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(height).value());
    EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(block_id).value());
    auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    EXPECT_FALSE(origin->has_value());

    EXPECT_FALSE(ctx.reconcile_outbox_tip(block_id, value)().has_value());
    EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(height).value());
    EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(block_id).value());
    origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    EXPECT_FALSE(origin->has_value());
  }

  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->origin.first_height, 1);
  EXPECT_EQ(current_origin->origin.first_prev_hash, sha256(genesis));
  EXPECT_TRUE(
      ctx.indexer_outbox_store().has_candidate(kHeightOneBlock).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(kMidchainBlock).value());

  EXPECT_EQ(mempool_rpc.eventsched2_commit_block(kHeightOneBlock, height_one)(),
            RPCError::kRPCNone);
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(1).value());
  EXPECT_FALSE(
      ctx.indexer_outbox_store().has_candidate(kHeightOneBlock).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(kMidchainBlock).value());
  EXPECT_EQ(midchain.proposal_header.block_height, 37);
}

TEST_F(EventRpcTest, ReconciliationDoesNotInitializeEmptyOriginAboveGenesis)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue tip{};
  tip.proposal_header.block_height = 1;

  EXPECT_FALSE(ctx.reconcile_outbox_tip(1, tip)().has_value());
  auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  EXPECT_FALSE(origin->has_value());
}

TEST_F(EventRpcTest, GenesisReconciliationAllowsNoncanonicalCandidateResidue)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(0)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_close_block(0, 0, ProposalTxs{})().error(),
            RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(0)(), RPCError::kRPCNone);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;

  EXPECT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->origin.first_height, 1);
  EXPECT_EQ(current_origin->origin.first_prev_hash, sha256(genesis));
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(0).value());
}

TEST_F(EventRpcTest, GenesisReconciliationRejectsCleanedTempResidue)
{
  {
    EventRPCContext ctx(temp_dir());
  }
  const std::string stale =
      IndexerOutboxStore::candidate_filename(1) + ".tmp.123.456";
  const std::filesystem::path path =
      std::filesystem::path(temp_dir()) / "indexer-outbox" / stale;
  ASSERT_TRUE(File::open(path.string(), O_CREAT | O_WRONLY, 0600).has_value());

  EventRPCContext restarted(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  EXPECT_FALSE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
  EXPECT_FALSE(std::filesystem::exists(path));
  auto origin = restarted.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  EXPECT_FALSE(origin->has_value());
}

TEST_F(EventRpcTest, GenesisReconciliationContinuesEmptyArchivalOrigin)
{
  {
    EventRPCContext ctx(temp_dir());
  }
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  write_epoch_origin_header(temp_dir(), "hogp/competing", 1, sha256(genesis));

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
  auto current = restarted.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(current.has_value()) << current.error();
  ASSERT_TRUE(current->has_value());
  const auto &current_value = current.value();
  const auto *current_origin =
      current_value.has_value() ? &current_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->epoch_sequence, 1UL);
  EXPECT_EQ(current_origin->origin.first_height, 1);
  EXPECT_EQ(current_origin->origin.first_prev_hash, sha256(genesis));
  auto origins = restarted.indexer_outbox_store().enumerate_epoch_origins_raw();
  ASSERT_TRUE(origins.has_value()) << origins.error();
  ASSERT_EQ(origins->size(), 2UL);
  EXPECT_EQ(origins->front().protocol, "hogp/competing");
}

TEST_F(EventRpcTest,
       StandaloneStartupDefersEventUntilGenesisAndRestartsAtBlockOne)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  ConsensusValue pregenesis{};
  pregenesis.proposal_header.block_height = -1;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = std::numeric_limits<BlockID>::max(),
                        .consensus_value = pregenesis});

  ConsensusValue block_one;
  constexpr BlockID kBlockOne = 790;
  {
    EventRPCContext ctx(temp_dir());
    EventMempoolRpc event_rpc(ctx);
    for (size_t retry = 0; retry < 2; ++retry) {
      EXPECT_EQ(MempoolEventRpc::schedevent2_prepare_event(card).outcome(),
                EventPrepareOutcome::EventPrepareNotReady);
    }
    EXPECT_FALSE(registration.event_registered(nullptr));
    auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    EXPECT_FALSE(origin->has_value());

    EventCard mismatched = card;
    mismatched.protocol = "hogp/not-this-binary";
    EXPECT_EQ(MempoolEventRpc::schedevent2_prepare_event(mismatched).outcome(),
              EventPrepareOutcome::EventPrepareError);

    ConsensusValue genesis{};
    genesis.proposal_header.block_height = 0;
    registration.publish_event_canonical_tip(
        EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
    EventPrepareResult prepared =
        MempoolEventRpc::schedevent2_prepare_event(card);
    ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
    ASSERT_TRUE(
        ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                 prepared.snapshot().tip.consensus_value)()
            .has_value());
    origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    ASSERT_TRUE(origin->has_value());
    const auto &origin_value = origin.value();
    const auto *current_origin =
        origin_value.has_value() ? &origin_value.value() : nullptr;
    ASSERT_NE(current_origin, nullptr);
    EXPECT_EQ(current_origin->origin.first_height, 1);
    EXPECT_EQ(current_origin->origin.first_prev_hash, sha256(genesis));
    ASSERT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);

    ASSERT_EQ(event_rpc.eventsched2_open_block(kBlockOne)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        event_rpc.eventsched2_close_block(kBlockOne, 1, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    block_one = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
    block_one.prev_hash = sha256(genesis);
    ASSERT_EQ(event_rpc.eventsched2_commit_block(kBlockOne, block_one)(),
              RPCError::kRPCNone);
    registration.publish_event_canonical_tip(
        EventCanonicalTip{.block_id = kBlockOne, .consensus_value = block_one});
  }

  // Simulate a full validator-node restart, including the mempool process.
  registration.clear();
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = kBlockOne, .consensus_value = block_one});
  EventRPCContext restarted(temp_dir());
  EventCard replacement{.protocol = version::kHogdProtocol(),
                        .address_for_exec = "127.0.0.1:3",
                        .address_for_sched = "127.0.0.1:4"};
  EventPrepareResult prepared =
      MempoolEventRpc::schedevent2_prepare_event(replacement);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  ASSERT_TRUE(
      restarted
          .reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                prepared.snapshot().tip.consensus_value)()
          .has_value());
  ASSERT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                .outcome(),
            EventActivationOutcome::EventActivationOK);
  EventCard registered;
  ASSERT_TRUE(registration.event_registered(&registered));
  EXPECT_EQ(registered, replacement);
}

TEST_F(EventRpcTest, GenesisRestartReconcilesBeforeEventActivation)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});

  EventCard first{.protocol = version::kHogdProtocol(),
                  .address_for_exec = "127.0.0.1:1",
                  .address_for_sched = "127.0.0.1:2"};
  EventPrepareResult prepared =
      MempoolEventRpc::schedevent2_prepare_event(first);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(
        ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                 prepared.snapshot().tip.consensus_value)()
            .has_value());
    ASSERT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);
  }

  registration.clear();
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
  EventCard replacement{.protocol = version::kHogdProtocol(),
                        .address_for_exec = "127.0.0.1:3",
                        .address_for_sched = "127.0.0.1:4"};
  prepared = MempoolEventRpc::schedevent2_prepare_event(replacement);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(
      restarted
          .reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                prepared.snapshot().tip.consensus_value)()
          .has_value());
  EXPECT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                .outcome(),
            EventActivationOutcome::EventActivationOK);
}

TEST_F(EventRpcTest,
       GenesisOriginRejectsMismatchedFirstCommitWithoutResolvingCandidate)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ProposalTxs proposal_txs;
  ConsensusValue correct;
  constexpr BlockID kBlock = 600;
  {
    EventRPCContext ctx(temp_dir());
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(kBlock)(), RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(kBlock, 1, proposal_txs)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    correct = make_consensus_value(1, closed.root_hash(), proposal_txs);
    correct.prev_hash = sha256(genesis);
    ConsensusValue mismatched = correct;
    mismatched.prev_hash[0] ^= 0xff;

    EXPECT_EQ(mempool_rpc.eventsched2_commit_block(kBlock, mismatched)(),
              RPCError::kRPCUnrecoverable);
    EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(1).value());
    EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(kBlock).value());
  }

  {
    EventRPCContext restarted(temp_dir());
    EventMempoolRpc mempool_rpc(restarted);
    ASSERT_TRUE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
    EXPECT_FALSE(restarted.indexer_outbox_store().has_committed(1).value());
    EXPECT_TRUE(restarted.indexer_outbox_store().has_candidate(kBlock).value());
    EXPECT_EQ(mempool_rpc.eventsched2_commit_block(kBlock, correct)(),
              RPCError::kRPCNone);
    EXPECT_TRUE(restarted.indexer_outbox_store().has_committed(1).value());
    EXPECT_FALSE(
        restarted.indexer_outbox_store().has_candidate(kBlock).value());
  }

  EventRPCContext reconciled(temp_dir());
  EXPECT_TRUE(reconciled.reconcile_outbox_tip(kBlock, correct)().has_value());
}

TEST_F(EventRpcTest, GenesisRegistrationRestartCommitsAndReconcilesHeightOne)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
  EventCard first{.protocol = version::kHogdProtocol(),
                  .address_for_exec = "127.0.0.1:1",
                  .address_for_sched = "127.0.0.1:2"};
  EventPrepareResult prepared =
      MempoolEventRpc::schedevent2_prepare_event(first);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(
        ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                 prepared.snapshot().tip.consensus_value)()
            .has_value());
    ASSERT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);
  }

  registration.clear();
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
  EventCard replacement{.protocol = version::kHogdProtocol(),
                        .address_for_exec = "127.0.0.1:3",
                        .address_for_sched = "127.0.0.1:4"};
  prepared = MempoolEventRpc::schedevent2_prepare_event(replacement);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  ConsensusValue height_one;
  constexpr BlockID kBlock = 601;
  {
    EventRPCContext restarted(temp_dir());
    EventMempoolRpc mempool_rpc(restarted);
    ASSERT_TRUE(
        restarted
            .reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                  prepared.snapshot().tip.consensus_value)()
            .has_value());
    ASSERT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(kBlock)(), RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(kBlock, 1, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    height_one = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
    height_one.prev_hash = sha256(genesis);
    ASSERT_EQ(mempool_rpc.eventsched2_commit_block(kBlock, height_one)(),
              RPCError::kRPCNone);
  }

  EventRPCContext reconciled(temp_dir());
  EXPECT_TRUE(
      reconciled.reconcile_outbox_tip(kBlock, height_one)().has_value());
}

TEST_F(EventRpcTest, ContiguousGenesisOutboxReopensAndReconciles)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue previous = genesis;
  constexpr BlockID kFirstBlock = 610;
  constexpr BlockID kSecondBlock = 611;
  {
    EventRPCContext ctx(temp_dir());
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    for (const auto &[block_id, height] :
         {std::pair{kFirstBlock, BlockHeight{1}},
          std::pair{kSecondBlock, BlockHeight{2}}}) {
      ASSERT_EQ(mempool_rpc.eventsched2_open_block(block_id)(),
                RPCError::kRPCNone);
      EventBlockResult closed =
          mempool_rpc.eventsched2_close_block(block_id, height,
                                              ProposalTxs{})();
      ASSERT_EQ(closed.error(), RPCError::kRPCNone);
      ConsensusValue value =
          make_consensus_value(height, closed.root_hash(), ProposalTxs{});
      value.prev_hash = sha256(previous);
      ASSERT_EQ(mempool_rpc.eventsched2_commit_block(block_id, value)(),
                RPCError::kRPCNone);
      previous = std::move(value);
    }
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(
      restarted.reconcile_outbox_tip(kSecondBlock, previous)().has_value());
}

TEST_F(EventRpcTest, ReconciliationIgnoresArchivalNoncanonicalCandidate)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(300)(), RPCError::kRPCNone);
  ASSERT_EQ(
      mempool_rpc.eventsched2_close_block(300, 30, ProposalTxs{})().error(),
      RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(300)(), RPCError::kRPCNone);
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::candidate_filename(300),
                          "hogp/archival");

  ConsensusValue no_blocks{};
  no_blocks.proposal_header.block_height = -1;
  EXPECT_TRUE(
      ctx.reconcile_outbox_tip(std::numeric_limits<BlockID>::max(), no_blocks)()
          .has_value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_candidate(300).value());
}

TEST_F(EventRpcTest, ReconciliationRejectsArchivalCanonicalCandidate)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;
  ASSERT_EQ(mempool_rpc.eventsched2_open_block(301)(), RPCError::kRPCNone);
  EventBlockResult closed =
      mempool_rpc.eventsched2_close_block(301, 31, proposal_txs)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(301)(), RPCError::kRPCNone);
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::candidate_filename(301),
                          "hogp/archival");
  ConsensusValue canonical =
      make_consensus_value(31, closed.root_hash(), proposal_txs);

  EXPECT_FALSE(ctx.reconcile_outbox_tip(301, canonical)().has_value());
}

TEST_F(EventRpcTest,
       ArchivalPrefixRestartInitializesCurrentOriginAndCommitsFirstBlock)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  auto archival_first = make_empty_committed(700, 20, ConsensusValueHash{});
  auto archival_tip =
      make_empty_committed(701, 21, sha256(archival_first.consensus_value));
  {
    EventRPCContext ctx(temp_dir());
    initialize_test_origin(ctx, archival_first.consensus_value);
    ASSERT_TRUE(ctx.indexer_outbox_store()
                    .publish_committed(archival_first)
                    .has_value());
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(archival_tip).has_value());
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival_a");
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(21),
                          "hogp/archival_a");
  replace_outbox_payload(temp_dir(), IndexerOutboxStore::committed_filename(20),
                         x23::Buf{0xff, 0xff, 0xff, 0xff});
  replace_outbox_payload(temp_dir(), IndexerOutboxStore::committed_filename(21),
                         x23::Buf{0xde, 0xad, 0xbe, 0xef});
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival_a", 20,
                            archival_first.consensus_value.prev_hash);
  replace_outbox_payload(temp_dir(),
                         IndexerOutboxStore::epoch_origin_filename(
                             "hogp/archival_a"),
                         x23::Buf{0x80, 0x00, 0x00, 0x01});

  registration.publish_event_canonical_tip(EventCanonicalTip{
      .block_id = 701,
      .consensus_value = archival_tip.consensus_value,
  });
  EventCard current_card{.protocol = version::kHogdProtocol(),
                         .address_for_exec = "127.0.0.1:1",
                         .address_for_sched = "127.0.0.1:2"};
  EventPrepareResult prepared =
      MempoolEventRpc::schedevent2_prepare_event(current_card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);

  ConsensusValue current_tip;
  constexpr BlockID kCurrentBlock = 702;
  {
    EventRPCContext restarted(temp_dir());
    ASSERT_TRUE(
        restarted
            .reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                  prepared.snapshot().tip.consensus_value)()
            .has_value());
    auto origin = restarted.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    ASSERT_TRUE(origin->has_value());
    const auto &origin_value = origin.value();
    const auto *current_origin =
        origin_value.has_value() ? &origin_value.value() : nullptr;
    ASSERT_NE(current_origin, nullptr);
    EXPECT_EQ(current_origin->origin.first_height, 22);
    EXPECT_EQ(current_origin->origin.first_prev_hash,
              sha256(archival_tip.consensus_value));
    ASSERT_EQ(MempoolEventRpc::schedevent2_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);

    EventMempoolRpc mempool_rpc(restarted);
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(kCurrentBlock)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(kCurrentBlock, 22, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    current_tip = make_consensus_value(22, closed.root_hash(), ProposalTxs{});
    current_tip.prev_hash = sha256(archival_tip.consensus_value);
    ASSERT_EQ(mempool_rpc.eventsched2_commit_block(kCurrentBlock,
                                                   current_tip)(),
              RPCError::kRPCNone);
  }

  EventRPCContext reconciled(temp_dir());
  ASSERT_TRUE(reconciled.reconcile_outbox_tip(kCurrentBlock, current_tip)()
                  .has_value());
  auto records =
      reconciled.indexer_outbox_store().enumerate_committed_records_raw();
  ASSERT_TRUE(records.has_value()) << records.error();
  ASSERT_EQ(records->size(), 3UL);
  EXPECT_EQ((*records)[0].protocol, "hogp/archival_a");
  EXPECT_EQ((*records)[1].protocol, "hogp/archival_a");
  EXPECT_EQ((*records)[2].protocol, std::string(version::kHogdProtocol()));
}

TEST_F(EventRpcTest, EmptyArchivalEpochAllowsNextEpochCommitAndRestart)
{
  auto archival = make_empty_committed(740, 20, ConsensusValueHash{});
  {
    EventRPCContext epoch_a(temp_dir());
    initialize_test_origin(epoch_a, archival.consensus_value);
    ASSERT_TRUE(
        epoch_a.indexer_outbox_store().publish_committed(archival).has_value());
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20), "hogp/a");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/a", 20,
                            archival.consensus_value.prev_hash,
                            /*epoch_sequence=*/0);

  {
    EventRPCContext epoch_b(temp_dir());
    ASSERT_TRUE(epoch_b.reconcile_outbox_tip(740, archival.consensus_value)()
                    .has_value());
  }
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/b", 21,
                            sha256(archival.consensus_value),
                            /*epoch_sequence=*/1);

  auto current =
      make_empty_committed(741, 21, sha256(archival.consensus_value));
  {
    EventRPCContext epoch_c(temp_dir());
    ASSERT_TRUE(epoch_c.reconcile_outbox_tip(740, archival.consensus_value)()
                    .has_value());
    ASSERT_TRUE(
        epoch_c.indexer_outbox_store().publish_committed(current).has_value());
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(741, current.consensus_value)()
                  .has_value());
}

TEST_F(EventRpcTest,
       AllEmptyEpochsPreserveGenesisBoundaryThroughCommitAndRestart)
{
  for (const size_t archival_epoch_count : {1UL, 2UL}) {
    TmpDir dir;
    ConsensusValue genesis{};
    genesis.proposal_header.block_height = 0;
    const ConsensusValueHash boundary_hash = sha256(genesis);
    {
      EventRPCContext create_namespace(dir.directory());
    }
    write_epoch_origin_header(dir.directory(), "hogp/a", 1, boundary_hash, 0);
    if (archival_epoch_count == 2) {
      write_epoch_origin_header(dir.directory(), "hogp/b", 1, boundary_hash, 1);
    }

    ConsensusValue committed;
    constexpr BlockID kBlock = 770;
    {
      EventRPCContext current(dir.directory());
      ASSERT_TRUE(current.reconcile_outbox_tip(0, genesis)().has_value());
      auto origin = current.indexer_outbox_store().load_current_epoch_origin();
      ASSERT_TRUE(origin.has_value()) << origin.error();
      ASSERT_TRUE(origin->has_value());
      const auto &origin_value = origin.value();
      const auto *current_origin =
          origin_value.has_value() ? &origin_value.value() : nullptr;
      ASSERT_NE(current_origin, nullptr);
      EXPECT_EQ(current_origin->epoch_sequence, archival_epoch_count);
      EXPECT_EQ(current_origin->origin.epoch_sequence, archival_epoch_count);
      EXPECT_EQ(current_origin->origin.first_height, 1);
      EXPECT_EQ(current_origin->origin.first_prev_hash, boundary_hash);

      EventMempoolRpc mempool_rpc(current);
      ASSERT_EQ(mempool_rpc.eventsched2_open_block(kBlock)(),
                RPCError::kRPCNone);
      EventBlockResult closed =
          mempool_rpc.eventsched2_close_block(kBlock, 1, ProposalTxs{})();
      ASSERT_EQ(closed.error(), RPCError::kRPCNone);
      committed = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
      committed.prev_hash = boundary_hash;
      ASSERT_EQ(mempool_rpc.eventsched2_commit_block(kBlock, committed)(),
                RPCError::kRPCNone);
    }

    EventRPCContext restarted(dir.directory());
    EXPECT_TRUE(
        restarted.reconcile_outbox_tip(kBlock, committed)().has_value());
    auto origins =
        restarted.indexer_outbox_store().enumerate_epoch_origins_raw();
    ASSERT_TRUE(origins.has_value()) << origins.error();
    ASSERT_EQ(origins->size(), archival_epoch_count + 1);
    for (const auto &origin : *origins) {
      EXPECT_EQ(origin.first_height, 1);
      EXPECT_EQ(origin.first_prev_hash, boundary_hash);
    }
  }
}

TEST_F(EventRpcTest, MalformedAllEmptyEpochBoundaryIsRejected)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  const ConsensusValueHash boundary_hash = sha256(genesis);
  {
    EventRPCContext create_namespace(temp_dir());
  }
  write_epoch_origin_header(temp_dir(), "hogp/a", 1, boundary_hash, 0);
  ConsensusValueHash wrong_boundary = boundary_hash;
  wrong_boundary[0] ^= 0xff;
  write_epoch_origin_header(temp_dir(), "hogp/b", 1, wrong_boundary, 1);

  EventRPCContext current(temp_dir());
  EXPECT_FALSE(current.reconcile_outbox_tip(0, genesis)().has_value());
  EXPECT_FALSE(
      current.indexer_outbox_store().load_current_epoch_origin()->has_value());
}

TEST_F(EventRpcTest, AllEmptyEpochBoundaryRejectsDisconnectedCandidatePromotion)
{
  constexpr BlockID kCandidate = 771;
  ConsensusValue candidate_tip;
  {
    EventRPCContext candidate_context(temp_dir());
    EventMempoolRpc mempool_rpc(candidate_context);
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(kCandidate)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(kCandidate, 1, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    candidate_tip = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
    candidate_tip.prev_hash[0] = 0x55;
  }

  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  write_epoch_origin_header(temp_dir(), "hogp/a", 1, sha256(genesis), 0);

  EventRPCContext restarted(temp_dir());
  EXPECT_FALSE(
      restarted.reconcile_outbox_tip(kCandidate, candidate_tip)().has_value());
  EXPECT_TRUE(
      restarted.indexer_outbox_store().has_candidate(kCandidate).value());
  EXPECT_FALSE(restarted.indexer_outbox_store()
                   .load_current_epoch_origin()
                   ->has_value());
}

TEST_F(EventRpcTest, MultipleEmptyArchivalEpochsPreserveCanonicalBoundary)
{
  auto archival = make_empty_committed(750, 20, ConsensusValueHash{});
  {
    EventRPCContext epoch_a(temp_dir());
    initialize_test_origin(epoch_a, archival.consensus_value);
    ASSERT_TRUE(
        epoch_a.indexer_outbox_store().publish_committed(archival).has_value());
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20), "hogp/a");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  const ConsensusValueHash boundary_hash = sha256(archival.consensus_value);
  write_epoch_origin_header(temp_dir(), "hogp/a", 20,
                            archival.consensus_value.prev_hash, 0);
  write_epoch_origin_header(temp_dir(), "hogp/b", 21, boundary_hash, 1);
  write_epoch_origin_header(temp_dir(), "hogp/c", 21, boundary_hash, 2);

  EventRPCContext epoch_d(temp_dir());
  ASSERT_TRUE(epoch_d.reconcile_outbox_tip(750, archival.consensus_value)()
                  .has_value());
  auto origins = epoch_d.indexer_outbox_store().enumerate_epoch_origins_raw();
  ASSERT_TRUE(origins.has_value()) << origins.error();
  ASSERT_EQ(origins->size(), 4UL);
  EXPECT_EQ(origins->back().epoch_sequence, 3UL);
  EXPECT_EQ(origins->back().first_height, 21);
  EXPECT_EQ(origins->back().first_prev_hash, boundary_hash);
}

TEST_F(EventRpcTest, MalformedEmptyArchivalEpochOriginIsRejected)
{
  auto archival = make_empty_committed(760, 20, ConsensusValueHash{});
  {
    EventRPCContext epoch_a(temp_dir());
    initialize_test_origin(epoch_a, archival.consensus_value);
    ASSERT_TRUE(
        epoch_a.indexer_outbox_store().publish_committed(archival).has_value());
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20), "hogp/a");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/a", 20,
                            archival.consensus_value.prev_hash, 0);
  ConsensusValueHash wrong_boundary = sha256(archival.consensus_value);
  wrong_boundary[0] ^= 0xff;
  write_epoch_origin_header(temp_dir(), "hogp/b", 21, wrong_boundary, 1);

  EventRPCContext epoch_c(temp_dir());
  EXPECT_FALSE(epoch_c.reconcile_outbox_tip(760, archival.consensus_value)()
                   .has_value());
  EXPECT_FALSE(
      epoch_c.indexer_outbox_store().load_current_epoch_origin()->has_value());
}

TEST_F(EventRpcTest,
       InvalidArchivalPrefixPreservesCandidateAndCreatesNoCurrentOrigin)
{
  auto verify = [&](bool corrupt_payload) {
    TmpDir dir;
    auto first = make_empty_committed(710, 20, ConsensusValueHash{});
    auto tip = make_empty_committed(711, 21, sha256(first.consensus_value));
    constexpr BlockID kCandidate = 712;
    {
      EventRPCContext ctx(dir.directory());
      initialize_test_origin(ctx, first.consensus_value);
      ASSERT_TRUE(
          ctx.indexer_outbox_store().publish_committed(first).has_value());
      ASSERT_TRUE(
          ctx.indexer_outbox_store().publish_committed(tip).has_value());
      EventMempoolRpc mempool_rpc(ctx);
      ASSERT_EQ(mempool_rpc.eventsched2_open_block(kCandidate)(),
                RPCError::kRPCNone);
      ASSERT_EQ(mempool_rpc
                    .eventsched2_close_block(kCandidate, 22, ProposalTxs{})()
                    .error(),
                RPCError::kRPCNone);
      ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(kCandidate)(),
                RPCError::kRPCNone);
    }
    if (corrupt_payload) {
      corrupt_outbox_payload(dir.directory(),
                             IndexerOutboxStore::committed_filename(20));
    }
    else {
      ConsensusValueHash disconnected = tip.consensus_value.prev_hash;
      disconnected[0] ^= 0xff;
      tip.consensus_value.prev_hash = disconnected;
      rewrite_committed_prev_hash(dir.directory(),
                                  IndexerOutboxStore::committed_filename(21),
                                  disconnected);
    }
    rewrite_outbox_protocol(dir.directory(),
                            IndexerOutboxStore::committed_filename(20),
                            "hogp/archival_bad");
    rewrite_outbox_protocol(dir.directory(),
                            IndexerOutboxStore::committed_filename(21),
                            "hogp/archival_bad");
    ASSERT_TRUE(std::filesystem::remove(
        std::filesystem::path(dir.directory()) / "indexer-outbox" /
        IndexerOutboxStore::current_epoch_origin_filename()));
    write_epoch_origin_header(dir.directory(), "hogp/archival_bad", 20,
                              first.consensus_value.prev_hash);

    EventRPCContext restarted(dir.directory());
    EXPECT_FALSE(
        restarted.reconcile_outbox_tip(711, tip.consensus_value)().has_value());
    auto current = restarted.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(current.has_value()) << current.error();
    EXPECT_FALSE(current->has_value());
    EXPECT_TRUE(
        restarted.indexer_outbox_store().has_candidate(kCandidate).value());
  };

  verify(/*corrupt_payload=*/false);
  verify(/*corrupt_payload=*/true);
}

TEST_F(EventRpcTest,
       InvalidArchivalPrefixDoesNotPromoteCanonicalRetainedCandidate)
{
  auto archival = make_empty_committed(720, 20, ConsensusValueHash{});
  constexpr BlockID kCandidate = 721;
  ConsensusValue canonical;
  {
    EventRPCContext ctx(temp_dir());
    initialize_test_origin(ctx, archival.consensus_value);
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(archival).has_value());
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(kCandidate)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(kCandidate, 21, ProposalTxs{})();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    canonical = make_consensus_value(21, closed.root_hash(), ProposalTxs{});
    canonical.prev_hash = sha256(archival.consensus_value);
    ASSERT_EQ(mempool_rpc.eventsched2_abandon_block(kCandidate)(),
              RPCError::kRPCNone);
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival_corrupt");
  corrupt_outbox_payload(temp_dir(),
                         IndexerOutboxStore::committed_filename(20));
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival_corrupt", 20,
                            archival.consensus_value.prev_hash);

  EventRPCContext restarted(temp_dir());
  EXPECT_FALSE(
      restarted.reconcile_outbox_tip(kCandidate, canonical)().has_value());
  EXPECT_FALSE(restarted.indexer_outbox_store().has_committed(21).value());
  EXPECT_TRUE(
      restarted.indexer_outbox_store().has_candidate(kCandidate).value());
  auto current = restarted.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(current.has_value()) << current.error();
  EXPECT_FALSE(current->has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsArchivalProtocolReentry)
{
  auto first = make_empty_committed(730, 20, ConsensusValueHash{});
  auto second = make_empty_committed(731, 21, sha256(first.consensus_value));
  auto third = make_empty_committed(732, 22, sha256(second.consensus_value));
  {
    EventRPCContext ctx(temp_dir());
    initialize_test_origin(ctx, first.consensus_value);
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(first).has_value());
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(second).has_value());
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(third).has_value());
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20), "hogp/a");
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(21), "hogp/b");
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(22), "hogp/a");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/a", 20,
                            first.consensus_value.prev_hash);
  write_epoch_origin_header(temp_dir(), "hogp/b", 21,
                            second.consensus_value.prev_hash,
                            /*epoch_sequence=*/1);

  EventRPCContext restarted(temp_dir());
  EXPECT_FALSE(
      restarted.reconcile_outbox_tip(732, third.consensus_value)().has_value());
  auto current = restarted.indexer_outbox_store().load_current_epoch_origin();
  ASSERT_TRUE(current.has_value()) << current.error();
  EXPECT_FALSE(current->has_value());
}

TEST_F(EventRpcTest, ReconciliationLinksCurrentEpochToArchivalPrefixByHeader)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(320)(), RPCError::kRPCNone);
  EventBlockResult archival_closed =
      mempool_rpc.eventsched2_close_block(320, 20, proposal_txs)();
  ASSERT_EQ(archival_closed.error(), RPCError::kRPCNone);
  ConsensusValue archival =
      make_consensus_value(20, archival_closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, archival);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(320, archival)(),
            RPCError::kRPCNone);

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(321)(), RPCError::kRPCNone);
  EventBlockResult current_closed =
      mempool_rpc.eventsched2_close_block(321, 21, proposal_txs)();
  ASSERT_EQ(current_closed.error(), RPCError::kRPCNone);
  ConsensusValue current =
      make_consensus_value(21, current_closed.root_hash(), proposal_txs);
  current.prev_hash = sha256(archival);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(321, current)(),
            RPCError::kRPCNone);

  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 20,
                            archival.prev_hash);
  write_epoch_origin_header(temp_dir(), version::kHogdProtocol(), 21,
                            current.prev_hash, 1);

  EXPECT_TRUE(ctx.reconcile_outbox_tip(999, current)().has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsDisconnectedArchivalPrefix)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(322)(), RPCError::kRPCNone);
  EventBlockResult archival_closed =
      mempool_rpc.eventsched2_close_block(322, 20, proposal_txs)();
  ASSERT_EQ(archival_closed.error(), RPCError::kRPCNone);
  ConsensusValue archival =
      make_consensus_value(20, archival_closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, archival);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(322, archival)(),
            RPCError::kRPCNone);

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(323)(), RPCError::kRPCNone);
  EventBlockResult current_closed =
      mempool_rpc.eventsched2_close_block(323, 21, proposal_txs)();
  ASSERT_EQ(current_closed.error(), RPCError::kRPCNone);
  ConsensusValue current =
      make_consensus_value(21, current_closed.root_hash(), proposal_txs);
  current.prev_hash = sha256(archival);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(323, current)(),
            RPCError::kRPCNone);
  current.prev_hash[0] ^= 0xff;
  ASSERT_NE(current.prev_hash, sha256(archival));
  rewrite_committed_prev_hash(temp_dir(),
                              IndexerOutboxStore::committed_filename(21),
                              current.prev_hash);

  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 20,
                            archival.prev_hash);
  write_epoch_origin_header(temp_dir(), version::kHogdProtocol(), 21,
                            current.prev_hash, 1);

  EXPECT_FALSE(ctx.reconcile_outbox_tip(323, current)().has_value());
}

TEST_F(EventRpcTest, ReconciliationAcceptsPreparedCurrentEpochOrigin)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue tip{};
  tip.proposal_header.block_height = 20;
  ASSERT_TRUE(ctx.indexer_outbox_store()
                  .initialize_current_epoch_origin(21, sha256(tip))
                  .has_value());

  EXPECT_TRUE(ctx.reconcile_outbox_tip(200, tip)().has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsInvalidPreparedCurrentEpochOrigin)
{
  ConsensusValue tip{};
  tip.proposal_header.block_height = 20;
  {
    TmpDir dir;
    EventRPCContext ctx(dir.directory());
    ASSERT_TRUE(ctx.indexer_outbox_store()
                    .initialize_current_epoch_origin(20, sha256(tip))
                    .has_value());
    EXPECT_FALSE(ctx.reconcile_outbox_tip(200, tip)().has_value());
  }
  {
    TmpDir dir;
    EventRPCContext ctx(dir.directory());
    ConsensusValueHash wrong_hash = sha256(tip);
    wrong_hash[0] ^= 0xff;
    ASSERT_TRUE(ctx.indexer_outbox_store()
                    .initialize_current_epoch_origin(21, wrong_hash)
                    .has_value());
    EXPECT_FALSE(ctx.reconcile_outbox_tip(200, tip)().has_value());
  }
}

TEST_F(EventRpcTest, ReconciliationValidatesCompleteMultiRecordArchivalEpoch)
{
  EventRPCContext ctx(temp_dir());
  auto first = make_empty_committed(330, 20, ConsensusValueHash{});
  auto second = make_empty_committed(331, 21, sha256(first.consensus_value));
  auto current = make_empty_committed(332, 22, sha256(second.consensus_value));
  initialize_test_origin(ctx, first.consensus_value);
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(first).has_value());
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(second).has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(current).has_value());
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(21),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 20,
                            first.consensus_value.prev_hash);
  write_epoch_origin_header(temp_dir(), version::kHogdProtocol(), 22,
                            current.consensus_value.prev_hash, 1);

  EXPECT_TRUE(
      ctx.reconcile_outbox_tip(999, current.consensus_value)().has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsArchivalInternalDisconnect)
{
  EventRPCContext ctx(temp_dir());
  auto first = make_empty_committed(333, 20, ConsensusValueHash{});
  ConsensusValueHash disconnected{};
  disconnected[0] = 0xff;
  auto second = make_empty_committed(334, 21, sha256(first.consensus_value));
  auto current = make_empty_committed(335, 22, sha256(second.consensus_value));
  initialize_test_origin(ctx, first.consensus_value);
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(first).has_value());
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(second).has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(current).has_value());
  second.consensus_value.prev_hash = disconnected;
  rewrite_committed_prev_hash(temp_dir(),
                              IndexerOutboxStore::committed_filename(21),
                              disconnected);
  current.consensus_value.prev_hash = sha256(second.consensus_value);
  rewrite_committed_prev_hash(temp_dir(),
                              IndexerOutboxStore::committed_filename(22),
                              current.consensus_value.prev_hash);
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(21),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 20,
                            first.consensus_value.prev_hash);
  write_epoch_origin_header(temp_dir(), version::kHogdProtocol(), 22,
                            current.consensus_value.prev_hash, 1);

  EXPECT_FALSE(
      ctx.reconcile_outbox_tip(335, current.consensus_value)().has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsTruncatedArchivalEpochPrefix)
{
  EventRPCContext ctx(temp_dir());
  auto first = make_empty_committed(336, 20, ConsensusValueHash{});
  auto second = make_empty_committed(337, 21, sha256(first.consensus_value));
  auto current = make_empty_committed(338, 22, sha256(second.consensus_value));
  initialize_test_origin(ctx, first.consensus_value);
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(first).has_value());
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(second).has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(current).has_value());
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(21),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 19,
                            first.consensus_value.prev_hash);
  write_epoch_origin_header(temp_dir(), version::kHogdProtocol(), 22,
                            current.consensus_value.prev_hash, 1);

  EXPECT_FALSE(
      ctx.reconcile_outbox_tip(338, current.consensus_value)().has_value());
}

TEST_F(EventRpcTest,
       ReconciliationPromotesRetainedCandidateWithRegeneratedBlockId)
{
  ProposalTxs proposal_txs;
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue ahead;
  x23::Buf expected_bytes;
  SHA256Hash expected_payload_digest;
  {
    EventRPCContext ctx(temp_dir());
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    ASSERT_EQ(mempool_rpc.eventsched2_open_block(210)(), RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched2_close_block(210, 1, proposal_txs)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    ahead = make_consensus_value(1, closed.root_hash(), proposal_txs);
    ahead.prev_hash = sha256(genesis);
    auto candidate = ctx.indexer_outbox_store().load_candidate(210);
    ASSERT_TRUE(candidate.has_value()) << candidate.error();
    IndexerOutboxCommittedPayload expected{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = ahead,
        .proposal_txs = candidate->proposal_txs,
        .event_logs = candidate->event_logs,
        .tx_bodies = candidate->tx_bodies,
    };
    expected_bytes = x23::serialize(expected);
    expected_payload_digest = sha256(expected);
    EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(1).value());
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.indexer_outbox_store().has_candidate(210).value());
  EXPECT_FALSE(restarted.indexer_outbox_store().has_candidate(310).value());
  EXPECT_FALSE(restarted.indexer_outbox_store().has_committed(1).value());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(310, ahead)().has_value());
  auto committed = restarted.indexer_outbox_store().load_committed(1);
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(x23::serialize(*committed), expected_bytes);
  EXPECT_EQ(sha256(*committed), expected_payload_digest);
  auto records =
      restarted.indexer_outbox_store().enumerate_committed_records_raw();
  ASSERT_TRUE(records.has_value()) << records.error();
  ASSERT_EQ(records->size(), 1UL);
  EXPECT_EQ(records->front().payload_digest, expected_payload_digest);
  EXPECT_FALSE(restarted.indexer_outbox_store().has_candidate(210).value());
  EXPECT_FALSE(restarted.indexer_outbox_store().has_candidate(310).value());
}

TEST_F(EventRpcTest, ReconciliationRejectsCorruptCurrentEpochPayload)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(214)(), RPCError::kRPCNone);
  EventBlockResult closed =
      mempool_rpc.eventsched2_close_block(214, 21, proposal_txs)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(21, closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(214, value)(),
            RPCError::kRPCNone);
  corrupt_outbox_payload(temp_dir(),
                         IndexerOutboxStore::committed_filename(21));

  EXPECT_FALSE(ctx.reconcile_outbox_tip(214, value)().has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsMismatchedCommittedRecordAhead)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;
  ConsensusValue scheduler_tip{};
  scheduler_tip.proposal_header.block_height = 20;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(211)(), RPCError::kRPCNone);
  EventBlockResult closed =
      mempool_rpc.eventsched2_close_block(211, 21, proposal_txs)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue ahead =
      make_consensus_value(21, closed.root_hash(), proposal_txs);
  ahead.prev_hash[0] = 0xff;
  initialize_test_origin(ctx, ahead);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(211, ahead)(),
            RPCError::kRPCNone);

  EXPECT_FALSE(ctx.reconcile_outbox_tip(200, scheduler_tip)().has_value());
}

TEST_F(EventRpcTest, ReconciliationRejectsTwoCommittedRecordsAhead)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;
  ConsensusValue scheduler_tip{};
  scheduler_tip.proposal_header.block_height = 20;

  ASSERT_EQ(mempool_rpc.eventsched2_open_block(212)(), RPCError::kRPCNone);
  EventBlockResult closed =
      mempool_rpc.eventsched2_close_block(212, 21, proposal_txs)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue first =
      make_consensus_value(21, closed.root_hash(), proposal_txs);
  first.prev_hash = sha256(scheduler_tip);
  initialize_test_origin(ctx, first);
  ASSERT_EQ(mempool_rpc.eventsched2_commit_block(212, first)(),
            RPCError::kRPCNone);
  auto first_record = ctx.indexer_outbox_store().load_committed(21);
  ASSERT_TRUE(first_record.has_value()) << first_record.error();
  IndexerOutboxCommittedPayload second = *first_record;
  second.consensus_value.proposal_header.block_height = 22;
  second.consensus_value.prev_hash = sha256(first);
  ASSERT_TRUE(ctx.indexer_outbox_store().publish_committed(second).has_value());

  EXPECT_FALSE(ctx.reconcile_outbox_tip(200, scheduler_tip)().has_value());
}

} // namespace hogd
