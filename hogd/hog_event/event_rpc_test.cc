#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono> // IWYU pragma: keep
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <latch>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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
#include "hogd/hog_event/event_block_context.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/hog_event/rpc_context.h"
#include "hogd/hog_event/rpc_from_exec.h"
#include "hogd/hog_event/rpc_from_indexer.h"
#include "hogd/hog_event/rpc_from_sched.h"
#include "hogd/hog_event/status.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/rpc_from_event.h"
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
  TxEventLog event{};
  event.tx_hash = sha256(body.signed_tx.tx);
  TxEmittedEvent emitted;
  emitted.contract_address[0] = marker;
  emitted.serialized_event.push_back(marker);
  event.events.push_back(std::move(emitted));
  return event;
}

ProposalTxs
make_proposal_txs(const ProposedTransactionBatch &bodies)
{
  x23::XdrVec<TransactionHash> hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  for (const ProposedTransaction &body : bodies) {
    hashes.push_back(sha256(body.signed_tx.tx));
    nondeterministic_data.push_back(body.nondeterministic_proposal_data);
  }
  ProposalTxs proposal_txs;
  append_to_parallel_batch(x23::span<const TransactionHash>(hashes),
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
  auto txs_root = compute_indexer_proposal_txs_root(proposal_txs)();
  CHECK(txs_root.has_value());
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal_txs);
  return value;
}

IndexerOutboxCommittedPayload
make_empty_committed(BlockHeight height, ConsensusValueHash prev_hash)
{
  ProposalTxs proposal_txs;
  TxEventLogBatch event_logs;
  ConsensusValue value =
      make_consensus_value(height,
                           compute_indexer_outbox_event_root(event_logs, {}),
                           proposal_txs);
  value.prev_hash = std::move(prev_hash);
  return {.format_version = kIndexerOutboxFormatVersion,
          .consensus_value = std::move(value),
          .event_logs = std::move(event_logs),
          .tx_bodies = {}};
}

void
initialize_test_origin(EventRPCContext &ctx, const ConsensusValue &first)
{
  ASSERT_TRUE(
      ctx.indexer_outbox_store()
          .initialize_current_epoch_origin(first.proposal_header.block_height,
                                           first.prev_hash)()
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

// Truncate a block's events file so reading it back fails. Dropping a byte
// leaves the final value short of a whole XDR word, which the bounds-checked
// pair walk rejects.
void
corrupt_events_file(std::string_view work_directory, SHA256Hash const &hash)
{
  const std::string path =
      EventBlockContext::final_events_filename(work_directory, hash);
  auto bytes = x23::readfile<x23::Buf>(path);
  ASSERT_TRUE(bytes.has_value());
  ASSERT_GT(bytes->size(), 1U);
  bytes->resize(bytes->size() - 1);
  auto file = File::open(path, O_TRUNC | O_WRONLY, 0600);
  ASSERT_TRUE(file.has_value());
  ASSERT_TRUE(write_full(*file, std::as_bytes(std::span(*bytes))).has_value());
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
  IndexerOutboxCommittedStoredPayload payload;
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

EventIndexerPullRequest
pull_request(IndexerProgress progress = IndexerProgress(
                 IndexerProgressState::IndexerProgressUninitialized))
{
  return {.protocol = version::kHogdProtocol(),
          .progress = std::move(progress)};
}

IndexerProgress
progress_at(const EventIndexerPulledBlock &pulled)
{
  IndexerProgress progress(IndexerProgressState::IndexerProgressInitialized);
  progress.initialized() = {
      .origin = pulled.origin,
      .contiguous_height =
          pulled.block.payload.consensus_value.proposal_header.block_height,
      .last_consensus_hash = pulled.block.identity.consensus_hash,
  };
  return progress;
}

ConsensusValue
commit_empty_block(EventRPCContext &ctx, BlockID block_id, BlockHeight height,
                   ConsensusValueHash prev_hash)
{
  EventMempoolRpc rpc(ctx);
  CHECK_EQ(rpc.eventsched_open_block(block_id)(), RPCError::kRPCNone);
  EventBlockResult closed = rpc.eventsched_close_block(block_id, height)();
  CHECK_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(height, closed.root_hash(), ProposalTxs{});
  value.prev_hash = std::move(prev_hash);
  CHECK_EQ(rpc.eventsched_commit_block(block_id, value)(), RPCError::kRPCNone);
  return value;
}

} // namespace

TEST_F(EventRpcTest, PullRejectsMismatchedProtocol)
{
  EventRPCContext ctx(temp_dir());
  EventIndexerRpc indexer_rpc(ctx);
  EventIndexerPullRequest request = pull_request();
  request.protocol = "definitely-not-the-hogd-protocol";

  EXPECT_EQ(indexer_rpc.event_indexer_pull_next(std::move(request))().outcome(),
            EventIndexerPullOutcome::EventIndexerPullMismatchedProtocol);
}

TEST_F(EventRpcTest, PullIsNotReadyBeforeReconciliation)
{
  EventRPCContext ctx(temp_dir());
  EXPECT_EQ(ctx.pull_indexer_next(pull_request())().outcome(),
            EventIndexerPullOutcome::EventIndexerPullNotReady);
}

TEST_F(EventRpcTest, FreshPullReturnsCurrentOriginOrFirstBlock)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  EventIndexerPullResult empty = ctx.pull_indexer_next(pull_request())();
  ASSERT_EQ(empty.outcome(), EventIndexerPullOutcome::EventIndexerPullCaughtUp);
  EXPECT_EQ(empty.origin().first_height, 1);
  EXPECT_EQ(empty.origin().first_prev_hash, sha256(genesis));

  IndexerProgress initialized(IndexerProgressState::IndexerProgressInitialized);
  initialized.initialized() = {
      .origin = empty.origin(),
      .contiguous_height = 0,
      .last_consensus_hash = sha256(genesis),
  };
  EventIndexerPullResult initialized_empty =
      ctx.pull_indexer_next(pull_request(initialized))();
  ASSERT_EQ(initialized_empty.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp)
      << std::string_view(initialized_empty.diagnostic());

  ConsensusValue block_one = commit_empty_block(ctx, 81, 1, sha256(genesis));
  EventIndexerPullResult first = ctx.pull_indexer_next(pull_request())();
  ASSERT_EQ(first.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(first.pulled_block().origin, empty.origin());
  EXPECT_EQ(first.pulled_block().block.payload.consensus_value, block_one);
}

TEST_F(EventRpcTest, ExactProgressReturnsNextAndUnchangedProgressRepeatsIt)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.indexer_outbox_store()
                  .initialize_current_epoch_origin(1, sha256(genesis))()
                  .has_value());
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  ConsensusValue one = commit_empty_block(ctx, 82, 1, sha256(genesis));

  EventIndexerPullResult first = ctx.pull_indexer_next(pull_request())();
  ASSERT_EQ(first.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  auto record = ctx.indexer_outbox_store().load_committed_record_info_raw(1);
  ASSERT_TRUE(record.has_value()) << record.error();
  IndexerProgress progress(IndexerProgressState::IndexerProgressInitialized);
  progress.initialized() = {
      .origin = first.pulled_block().origin,
      .contiguous_height = 1,
      .last_consensus_hash = record->consensus_hash,
  };
  EventIndexerPullResult caught_up =
      ctx.pull_indexer_next(pull_request(progress))();
  ASSERT_EQ(caught_up.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp)
      << std::string_view(caught_up.diagnostic());

  ConsensusValue two = commit_empty_block(ctx, 83, 2, sha256(one));
  EventIndexerPullResult next = ctx.pull_indexer_next(pull_request(progress))();
  EventIndexerPullResult repeated =
      ctx.pull_indexer_next(pull_request(progress))();

  ASSERT_EQ(next.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock)
      << std::string_view(next.diagnostic());
  ASSERT_EQ(repeated.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(next.pulled_block().block.payload.consensus_value, two);
  EXPECT_EQ(x23::serialize(next.pulled_block()),
            x23::serialize(repeated.pulled_block()));
}

TEST_F(EventRpcTest, ForgedAndUnknownProgressConflict)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.indexer_outbox_store()
                  .initialize_current_epoch_origin(1, sha256(genesis))()
                  .has_value());
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  EventIndexerPullResult caught_up = ctx.pull_indexer_next(pull_request())();
  ASSERT_EQ(caught_up.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp);

  IndexerProgress boundary(IndexerProgressState::IndexerProgressInitialized);
  boundary.initialized() = {
      .origin = caught_up.origin(),
      .contiguous_height = 0,
      .last_consensus_hash = sha256(genesis),
  };
  IndexerProgress unknown = boundary;
  unknown.initialized().origin.protocol = "hogp/unknown";
  EXPECT_EQ(ctx.pull_indexer_next(pull_request(unknown))().outcome(),
            EventIndexerPullOutcome::EventIndexerPullProgressConflict);

  commit_empty_block(ctx, 821, 1, sha256(genesis));
  EventIndexerPullResult first = ctx.pull_indexer_next(pull_request())();
  ASSERT_EQ(first.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  IndexerProgress forged = progress_at(first.pulled_block());
  forged.initialized().last_consensus_hash[0] ^= 1;
  EXPECT_EQ(ctx.pull_indexer_next(pull_request(forged))().outcome(),
            EventIndexerPullOutcome::EventIndexerPullProgressConflict);
}

TEST_F(EventRpcTest, FinalizedCommitMakesRecordPullable)
{
  EventRPCContext ctx(temp_dir());
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
  ConsensusValue block = commit_empty_block(ctx, 861, 1, sha256(genesis));

  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(1).value_or(false));
  EXPECT_EQ(ctx.pull_indexer_next(pull_request())().outcome(),
            EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(ctx.pull_indexer_next(pull_request())()
                .pulled_block()
                .block.payload.consensus_value,
            block);
}

TEST_F(EventRpcTest, FailedCommittedPublishPreservesValidatedPulls)
{
  constexpr std::array kFaults{
      IndexerOutboxFaultPoint::kVerifyPublishedInode,
      IndexerOutboxFaultPoint::kClosePublishedTemp,
      IndexerOutboxFaultPoint::kUnlinkPublishedTemp,
      IndexerOutboxFaultPoint::kFsyncPublishedDirectory,
  };
  for (size_t i = 0; i < kFaults.size(); ++i) {
    TmpDir dir;
    EventRPCContext ctx(dir.directory());
    EventMempoolRpc event_rpc(ctx);
    ConsensusValue genesis{};
    genesis.proposal_header.block_height = 0;
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    ConsensusValue block_one =
        commit_empty_block(ctx, 870 + (i * 2), 1, sha256(genesis));
    EventIndexerPullResult first = ctx.pull_indexer_next(pull_request())();
    ASSERT_EQ(first.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
    IndexerProgress first_progress = progress_at(first.pulled_block());

    const BlockID block_two_id = 871 + (i * 2);
    ASSERT_EQ(event_rpc.eventsched_open_block(block_two_id)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        event_rpc.eventsched_close_block(block_two_id, 2)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    ConsensusValue block_two =
        make_consensus_value(2, closed.root_hash(), ProposalTxs{});
    block_two.prev_hash = sha256(block_one);

    fail_next_indexer_outbox_operation_for_test(kFaults[i]);
    EXPECT_EQ(event_rpc.eventsched_commit_block(block_two_id, block_two)(),
              RPCError::kRPCUnrecoverable);
    ASSERT_TRUE(ctx.indexer_outbox_store().has_committed(2).value_or(false));
    EXPECT_EQ(ctx.pull_indexer_next(pull_request(first_progress))().outcome(),
              EventIndexerPullOutcome::EventIndexerPullCaughtUp);

    ASSERT_EQ(event_rpc.eventsched_commit_block(block_two_id, block_two)(),
              RPCError::kRPCNone);
    EventIndexerPullResult second =
        ctx.pull_indexer_next(pull_request(first_progress))();
    ASSERT_EQ(second.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
    EXPECT_EQ(second.pulled_block().block.payload.consensus_value, block_two);
  }
}

TEST_F(EventRpcTest, ReconciledFinalizedOneAheadRecordRemainsPullable)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue block;
  {
    EventRPCContext first(temp_dir());
    ASSERT_TRUE(first.reconcile_outbox_tip(0, genesis)().has_value());
    block = commit_empty_block(first, 842, 1, sha256(genesis));
  }

  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(restarted.reconcile_outbox_tip(0, genesis)().has_value());
  EventIndexerPullResult pulled = restarted.pull_indexer_next(pull_request())();
  ASSERT_EQ(pulled.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(pulled.pulled_block().block.payload.consensus_value, block);
  ASSERT_TRUE(restarted.commit_block(842, block)().has_value());
  EXPECT_EQ(restarted.pull_indexer_next(pull_request())().outcome(),
            EventIndexerPullOutcome::EventIndexerPullBlock);
}

TEST_F(EventRpcTest, CommitRejectsConflictingAndOutOfOrderValues)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc event_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ASSERT_EQ(event_rpc.eventsched_open_block(843)(), RPCError::kRPCNone);
  EventBlockResult first_closed = event_rpc.eventsched_close_block(843, 1)();
  ASSERT_EQ(first_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, first_closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched_open_block(844)(), RPCError::kRPCNone);
  EventBlockResult second_closed = event_rpc.eventsched_close_block(844, 2)();
  ASSERT_EQ(second_closed.error(), RPCError::kRPCNone);
  ConsensusValue block_two =
      make_consensus_value(2, second_closed.root_hash(), ProposalTxs{});
  block_two.prev_hash = sha256(block_one);
  ConsensusValue conflicting = block_one;
  conflicting.prev_hash[0] ^= 1;
  EXPECT_EQ(event_rpc.eventsched_commit_block(844, block_two)(),
            RPCError::kRPCUnrecoverable);
  ASSERT_EQ(event_rpc.eventsched_commit_block(843, block_one)(),
            RPCError::kRPCNone);
  EXPECT_EQ(event_rpc.eventsched_commit_block(843, conflicting)(),
            RPCError::kRPCBadBlockID);
  ASSERT_EQ(event_rpc.eventsched_commit_block(844, block_two)(),
            RPCError::kRPCNone);
  EXPECT_EQ(event_rpc.eventsched_commit_block(843, block_one)(),
            RPCError::kRPCUnrecoverable);
  EXPECT_EQ(event_rpc.eventsched_commit_block(844, block_two)(),
            RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TxFailureMailboxDrainsIsBoundedAndDoesNotBlockIngress)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  TxFailureDiagnostic diag;
  diag.attempt_height = 42;
  diag.conflict.code = DbConflictCode::kDbConflictReplayProtection;
  for (size_t i = 0; i < EventRPCContext::kMaxTxFailureNotices + 32; ++i) {
    TransactionHash tx_hash{};
    tx_hash[0] = static_cast<uint8_t>(i);
    ASSERT_EQ(exec_rpc.eventexecutor_report_tx_failure(tx_hash, diag)(),
              RPCError::kRPCNone);
  }

  ASSERT_EQ(mempool_rpc.eventsched_open_block(91)(), RPCError::kRPCNone);
  ASSERT_EQ(mempool_rpc.eventsched_abandon_block(91)(), RPCError::kRPCNone);
  EventIndexerRpc indexer_rpc(ctx);
  IndexerTxFailureNoticeBatch notices =
      indexer_rpc.event_indexer_pull_tx_failures();
  ASSERT_EQ(notices.size(), EventRPCContext::kMaxTxFailureNotices);
  EXPECT_EQ(notices.front().diagnostic.attempt_height, 42);
  EXPECT_TRUE(indexer_rpc.event_indexer_pull_tx_failures().empty());
}

TEST_F(EventRpcTest, MissingOrCorruptPullableRecordIsInternalFailure)
{
  for (const bool corrupt : {false, true}) {
    TmpDir dir;
    EventRPCContext ctx(dir.directory());
    ConsensusValue genesis{};
    genesis.proposal_header.block_height = 0;
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    commit_empty_block(ctx, corrupt ? 92 : 93, 1, sha256(genesis));
    const std::filesystem::path record =
        std::filesystem::path(dir.directory()) / "indexer-outbox" /
        IndexerOutboxStore::committed_filename(1);
    if (corrupt) {
      corrupt_outbox_payload(dir.directory(),
                             IndexerOutboxStore::committed_filename(1));
    }
    else {
      ASSERT_TRUE(std::filesystem::remove(record));
    }

    EXPECT_EQ(ctx.pull_indexer_next(pull_request())().outcome(),
              EventIndexerPullOutcome::EventIndexerPullInternalFailure);
  }
}

TEST_F(EventRpcTest, PullTransitionsFromRetainedToCurrentEpoch)
{
  auto archival = make_empty_committed(20, ConsensusValueHash{});
  {
    EventRPCContext previous(temp_dir());
    initialize_test_origin(previous, archival.consensus_value);
    ASSERT_TRUE(previous.indexer_outbox_store()
                    .publish_committed(archival)()
                    .has_value());
    auto records =
        previous.indexer_outbox_store().enumerate_committed_records_raw();
    ASSERT_TRUE(records.has_value()) << records.error();
    ASSERT_EQ(records->size(), 1UL);
  }
  rewrite_outbox_protocol(temp_dir(),
                          IndexerOutboxStore::committed_filename(20),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 20,
                            archival.consensus_value.prev_hash);

  EventRPCContext current(temp_dir());
  ASSERT_TRUE(
      current.reconcile_outbox_tip(94, archival.consensus_value)().has_value());
  IndexerProgress progress(IndexerProgressState::IndexerProgressInitialized);
  progress.initialized() = {
      .origin =
          IndexerOutboxOrigin{
              .protocol = "hogp/archival",
              .epoch_sequence = 0,
              .first_height = 20,
              .first_prev_hash = archival.consensus_value.prev_hash,
          },
      .contiguous_height = 20,
      .last_consensus_hash = sha256(archival.consensus_value),
  };
  IndexerProgress bad_boundary = progress;
  bad_boundary.initialized().last_consensus_hash[0] ^= 1;
  EXPECT_EQ(current.pull_indexer_next(pull_request(bad_boundary))().outcome(),
            EventIndexerPullOutcome::EventIndexerPullProgressConflict);
  EventIndexerPullResult at_boundary =
      current.pull_indexer_next(pull_request(progress))();
  ASSERT_EQ(at_boundary.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp);
  EXPECT_EQ(at_boundary.origin().epoch_sequence, 1UL);

  ConsensusValue block =
      commit_empty_block(current, 95, 21, sha256(archival.consensus_value));
  EventIndexerPullResult transitioned =
      current.pull_indexer_next(pull_request(progress))();
  ASSERT_EQ(transitioned.outcome(),
            EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(transitioned.pulled_block().origin, at_boundary.origin());
  EXPECT_EQ(transitioned.pulled_block().block.payload.consensus_value, block);
}

TEST_F(EventRpcTest, FreshPullStartsAtOldestRetainedEpoch)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  const ConsensusValueHash boundary = sha256(genesis);
  auto archival = make_empty_committed(1, boundary);
  {
    EventRPCContext previous(temp_dir());
    initialize_test_origin(previous, archival.consensus_value);
    ASSERT_TRUE(previous.indexer_outbox_store()
                    .publish_committed(archival)()
                    .has_value());
  }
  rewrite_outbox_protocol(temp_dir(), IndexerOutboxStore::committed_filename(1),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 1, boundary);

  EventRPCContext current(temp_dir());
  ASSERT_TRUE(
      current.reconcile_outbox_tip(96, archival.consensus_value)().has_value());
  EventIndexerPullResult first = current.pull_indexer_next(pull_request())();
  ASSERT_EQ(first.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock)
      << std::string_view(first.diagnostic());
  EXPECT_EQ(std::string_view(first.pulled_block().origin.protocol),
            "hogp/archival");
  EXPECT_EQ(first.pulled_block().block.payload.consensus_value,
            archival.consensus_value);

  EventIndexerPullResult transitioned = current.pull_indexer_next(
      pull_request(progress_at(first.pulled_block())))();
  ASSERT_EQ(transitioned.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp)
      << std::string_view(transitioned.diagnostic());
  EXPECT_EQ(transitioned.origin().epoch_sequence, 1UL);
  EXPECT_EQ(std::string_view(transitioned.origin().protocol),
            version::kHogdProtocol());
}

TEST_F(EventRpcTest, PullAdvancesThroughConsecutiveEmptyEpochs)
{
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  const ConsensusValueHash boundary = sha256(genesis);
  {
    EventRPCContext create_namespace(temp_dir());
  }
  write_epoch_origin_header(temp_dir(), "hogp/a", 1, boundary, 0);
  write_epoch_origin_header(temp_dir(), "hogp/b", 1, boundary, 1);

  EventRPCContext current(temp_dir());
  ASSERT_TRUE(current.reconcile_outbox_tip(0, genesis)().has_value());
  ConsensusValue block = commit_empty_block(current, 97, 1, boundary);
  EventIndexerPullResult epoch_a = current.pull_indexer_next(pull_request())();
  ASSERT_EQ(epoch_a.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp);
  EXPECT_EQ(std::string_view(epoch_a.origin().protocol), "hogp/a");

  const auto boundary_progress = [&](const IndexerOutboxOrigin &origin) {
    IndexerProgress progress(IndexerProgressState::IndexerProgressInitialized);
    progress.initialized() = {
        .origin = origin,
        .contiguous_height = 0,
        .last_consensus_hash = boundary,
    };
    return progress;
  };
  EventIndexerPullResult epoch_b = current.pull_indexer_next(
      pull_request(boundary_progress(epoch_a.origin())))();
  ASSERT_EQ(epoch_b.outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp)
      << std::string_view(epoch_b.diagnostic());
  EXPECT_EQ(std::string_view(epoch_b.origin().protocol), "hogp/b");

  EventIndexerPullResult first_block = current.pull_indexer_next(
      pull_request(boundary_progress(epoch_b.origin())))();
  ASSERT_EQ(first_block.outcome(),
            EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(first_block.pulled_block().origin.epoch_sequence, 2UL);
  EXPECT_EQ(std::string_view(first_block.pulled_block().origin.protocol),
            version::kHogdProtocol());
  EXPECT_EQ(first_block.pulled_block().block.payload.consensus_value, block);
}

TEST_F(EventRpcTest, DuplicateOpenReturnsExists)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(3)(), RPCError::kRPCNone);

  // A second open of a live block id must fail cleanly with kExists (not
  // abort constructing-then-destroying an active context).
  auto status = ctx.open_block(3)();
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), EventErrorCode::kExists);

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(3)(), RPCError::kRPCNone);
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

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  ProposedTransaction body = make_body(1);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 1),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(2, 2, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);

  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1, 1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestAbandonBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestEmptyBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  ProposalTxs proposal_txs;
  EventBlockResult close_result = mempool_rpc.eventsched_close_block(1, 1)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(1, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(1, value)(),
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
                            ->client<EventSched1>();
  EXPECT_NE(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);

  EXPECT_NE(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{},
                                                 ProposedTransaction{}),
            RPCError::kRPCNone);

  EXPECT_EQ(mempool_client.eventsched_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);

  ProposedTransaction body = make_body(10);
  EXPECT_EQ(exec_client.eventexecutor_emit_event(0, 0, make_event(body, 10),
                                                 body),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  EventBlockResult close_result = mempool_client.eventsched_close_block(0, 0);
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(0, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_client.eventsched_commit_block(0, value),
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
                            ->client<EventSched1>();

  EXPECT_EQ(mempool_client.eventsched_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  ProposalTxs proposal_txs;
  EventBlockResult close_result = mempool_client.eventsched_close_block(0, 0);
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(0, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_client.eventsched_commit_block(0, value),
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
                            ->client<EventSched1>();

  EXPECT_EQ(mempool_client.eventsched_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_open_block(0), RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  ProposalTxs proposal_txs;
  EventBlockResult close_result = mempool_client.eventsched_close_block(0, 0);
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(0, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_client.eventsched_commit_block(0, value),
            RPCError::kRPCNone);
}

// Test multiple executors with proper reference counting
TEST_F(EventRpcTest, MultipleExecutors)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);

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
    auto result = co_await mempool_rpc.eventsched_close_block(1, 1);
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

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
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
    auto result = co_await mempool_rpc.eventsched_close_block(1, 1);
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
  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
  // should return an error because the block is abandoned
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCBadBlockID);
}

// Test abandoning a block while close_block is waiting for executors
TEST_F(EventRpcTest, AbandonIsRejectedWhileCloseBlockIsReserved)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Start close_block in background (will wait for executor)
  std::atomic<bool> close_completed = false;
  std::latch close_reserved(1);
  ctx.set_close_reserved_test_hook(
      [&](BlockID /*block_id*/) { close_reserved.count_down(); });
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    auto result = co_await mempool_rpc.eventsched_close_block(1, 1);
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    close_completed.store(true);
  }();

  close_reserved.wait();

  // The close reservation prevents abandon from destructively racing it.
  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(),
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

  ASSERT_EQ(mempool_rpc.eventsched_open_block(2)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(2)(), RPCError::kRPCNone);

  std::latch close_reserved(1);
  ctx.set_close_reserved_test_hook(
      [&](BlockID /*block_id*/) { close_reserved.count_down(); });
  auto close_async = mempool_rpc.eventsched_close_block(2, 2);

  close_reserved.wait();
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(
                2, make_consensus_value(2, {}, proposal_txs))(),
            RPCError::kRPCBadBlockID);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(2)(), RPCError::kRPCNone);
  EventBlockResult close_result = close_async();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(2, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(2, value)(),
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

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Models an in-flight executor RPC holding the block's context.
  auto held_ref = ctx.get_context(1)();
  ASSERT_TRUE(held_ref);

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);

  EXPECT_EQ(mempool_rpc.eventsched_close_block(1, 1)().error(),
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
    ASSERT_EQ(mempool_rpc.eventsched_open_block(block_id)(),
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
      auto result = co_await mempool_rpc.eventsched_close_block(block_id, 1);
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
    RPCError abandon_err = mempool_rpc.eventsched_abandon_block(block_id)();

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
    EXPECT_EQ(mempool_rpc.eventsched_open_block(block_id)(),
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

    auto result = mempool_rpc.eventsched_close_block(block_id, block_id)();
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    ConsensusValue value =
        make_consensus_value(block_id, result.root_hash(), proposal_txs);
    if (block_id > 1) {
      value.prev_hash = sha256(previous);
    }
    else {
      initialize_test_origin(ctx, value);
    }
    EXPECT_EQ(mempool_rpc.eventsched_commit_block(block_id, value)(),
              RPCError::kRPCNone);
    previous = std::move(value);
  }
}

TEST_F(EventRpcTest, AbandonActiveBlockDeletesTempFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);

  auto temp_file = EventBlockContext::temp_events_filename(temp_dir(), 1);
  EXPECT_TRUE(std::filesystem::exists(temp_file));

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_FALSE(std::filesystem::exists(temp_file));
}

TEST_F(EventRpcTest, AbandonClosedBlockReclaimsUnsharedFinalFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  ProposedTransaction body = make_body(31);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 31),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1, 1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  auto final_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());
  auto temp_file = EventBlockContext::temp_events_filename(temp_dir(), 1);

  // A block is abandoned because a sibling committed, so it lost and its
  // events file is dead.  No other live block shares this root, so the last
  // claim on the name goes away with the context and the file is reclaimed.
  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_FALSE(std::filesystem::exists(final_file));
  EXPECT_FALSE(std::filesystem::exists(temp_file));
}

TEST_F(EventRpcTest, CommitBlockRetainsFinalFileAsRecordPayload)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  ProposedTransaction body = make_body(32);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 32),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1, 1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  auto final_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());

  ConsensusValue value =
      make_consensus_value(1, result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(1, value)(),
            RPCError::kRPCNone);
  // The committed record references the events file rather than embedding a
  // second copy of the pairs, so commit takes a permanent claim on it and the
  // block context's teardown must not reclaim it.
  EXPECT_TRUE(std::filesystem::exists(final_file));
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(1).value());
}

TEST_F(EventRpcTest, CloseWritesEventsFileAndCommitPromotesIt)
{
  ProposedTransaction body = make_body(41);
  TxEventLog event = make_event(body, 41);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  ConsensusValue consensus_value;

  {
    EventRPCContext ctx(temp_dir());
    EventExecRpc exec_rpc(ctx);
    EventMempoolRpc mempool_rpc(ctx);

    ASSERT_EQ(mempool_rpc.eventsched_open_block(41)(), RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_open_block(41)(), RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_emit_event(41, 1, event, body)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_close_block(41)(), RPCError::kRPCNone);

    EventBlockResult close_result = mempool_rpc.eventsched_close_block(41, 9)();
    ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
    // Close leaves exactly one durable artifact: the events file, content
    // addressed by the event root it just returned, holding every event log
    // paired with the body that produced it.
    auto pairs = read_final_events_file(temp_dir(), close_result.root_hash());
    ASSERT_TRUE(pairs.has_value()) << pairs.error();
    ASSERT_EQ(pairs->event_logs.size(), 1);
    ASSERT_EQ(pairs->tx_bodies.size(), 1);
    expect_xdr_equal(pairs->event_logs[0], event);
    expect_xdr_equal(pairs->tx_bodies[0], body);

    consensus_value =
        make_consensus_value(9, close_result.root_hash(), proposal_txs);
    consensus_value.proposal_header.block_timepoint = 12345;
    initialize_test_origin(ctx, consensus_value);
    ASSERT_EQ(mempool_rpc.eventsched_commit_block(41, consensus_value)(),
              RPCError::kRPCNone);

    // The committed record references this file as its payload, so promotion
    // retains it for as long as the record lives.
    EXPECT_TRUE(std::filesystem::exists(
        EventBlockContext::final_events_filename(temp_dir(),
                                                 close_result.root_hash())));
    auto committed = ctx.indexer_outbox_store().load_committed(9)();
    ASSERT_TRUE(committed.has_value()) << committed.error();
    expect_xdr_equal(committed->consensus_value, consensus_value);
    expect_xdr_equal(committed->event_logs[0], event);
    expect_xdr_equal(committed->tx_bodies[0], body);
  }

  auto reopened = IndexerOutboxStore::open(temp_dir() + "/indexer-outbox");
  ASSERT_TRUE(reopened.has_value()) << reopened.error();
  auto committed = reopened->load_committed(9)();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  expect_xdr_equal(committed->consensus_value, consensus_value);
}

TEST_F(EventRpcTest, AbandonClosedBlockReclaimsItsEventsFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(42);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched_open_block(42)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(42)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(42, 1, make_event(body, 42),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(42)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(42, 10)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  const std::string events_file =
      EventBlockContext::final_events_filename(temp_dir(), closed.root_hash());
  ASSERT_TRUE(std::filesystem::exists(events_file));

  // Abandonment follows a sibling committing at this height, so this block
  // will never be promoted and nothing else holds its root.
  ASSERT_EQ(mempool_rpc.eventsched_abandon_block(42)(), RPCError::kRPCNone);
  EXPECT_FALSE(std::filesystem::exists(events_file));
  EXPECT_FALSE(ctx.indexer_outbox_store().load_committed(10)().has_value());
}

TEST_F(EventRpcTest, EmptyBlockWritesEventsFileAndCommittedRecord)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched_open_block(43)(), RPCError::kRPCNone);
  EventBlockResult close_result = mempool_rpc.eventsched_close_block(43, 11)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  auto pairs = read_final_events_file(temp_dir(), close_result.root_hash());
  ASSERT_TRUE(pairs.has_value()) << pairs.error();
  EXPECT_TRUE(pairs->event_logs.empty());
  EXPECT_TRUE(pairs->tx_bodies.empty());

  ConsensusValue value =
      make_consensus_value(11, close_result.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(43, value)(),
            RPCError::kRPCNone);
  auto committed = ctx.indexer_outbox_store().load_committed(11)();
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
    ASSERT_EQ(mempool_rpc.eventsched_open_block(block_id)(),
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
  EventBlockResult first_closed = mempool_rpc.eventsched_close_block(430, 1)();
  EventBlockResult second_closed = mempool_rpc.eventsched_close_block(431, 1)();
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

  auto first_commit = mempool_rpc.eventsched_commit_block(430, first);
  auto second_commit = mempool_rpc.eventsched_commit_block(431, second);
  const RPCError first_result = first_commit();
  const RPCError second_result = second_commit();
  EXPECT_NE(first_result == RPCError::kRPCNone,
            second_result == RPCError::kRPCNone);

  auto committed = ctx.indexer_outbox_store().load_committed(1)();
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

TEST_F(EventRpcTest, CommitDoesNotInterpretProposalSegmentationCommitment)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(44);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched_open_block(44)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(44)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(44, 1, make_event(body, 44),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(44)(), RPCError::kRPCNone);
  EventBlockResult close_result = mempool_rpc.eventsched_close_block(44, 12)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);

  ConsensusValue mismatched =
      make_consensus_value(12, close_result.root_hash(), proposal_txs);
  mismatched.proposal_txs_hash[0] ^= 0x01;
  initialize_test_origin(ctx, mismatched);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(44, mismatched)(),
            RPCError::kRPCNone);
  EXPECT_TRUE(ctx.indexer_outbox_store().load_committed(12)().has_value());
  EXPECT_FALSE(ctx.indexer_outbox_store().load_candidate(44)().has_value());
}

TEST_F(EventRpcTest, IdenticalCommittedPrepublicationIsIdempotentAcrossBlockIds)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched_open_block(45)(), RPCError::kRPCNone);
  EventBlockResult close_result = mempool_rpc.eventsched_close_block(45, 13)();
  ASSERT_EQ(close_result.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(13, close_result.root_hash(), proposal_txs);
  auto pairs = read_final_events_file(temp_dir(), close_result.root_hash());
  ASSERT_TRUE(pairs.has_value()) << pairs.error();
  IndexerOutboxCommittedPayload committed{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = value,
      .event_logs = pairs->event_logs,
      .tx_bodies = pairs->tx_bodies,
  };
  initialize_test_origin(ctx, value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(committed)().has_value());

  EXPECT_EQ(mempool_rpc.eventsched_commit_block(45, value)(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(99, value)(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(45, value)(),
            RPCError::kRPCNone);
  auto loaded = ctx.indexer_outbox_store().load_committed(13)();
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  expect_xdr_equal(*loaded, committed);
}

TEST_F(EventRpcTest, PrepublishedConsensusRetryServesTheRecord)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(451);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched_open_block(451)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(451)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(451, 1, make_event(body, 0x45),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(451)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(451, 13)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(13, closed.root_hash(), proposal_txs);
  auto original = read_final_events_file(temp_dir(), closed.root_hash());
  ASSERT_TRUE(original.has_value()) << original.error();
  IndexerOutboxCommittedPayload committed{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = value,
      .event_logs = original->event_logs,
      .tx_bodies = original->tx_bodies,
  };
  initialize_test_origin(ctx, value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(committed)().has_value());

  // Once a committed record exists for the height, a retry of the same
  // consensus value returns it. The record stores only the consensus value and
  // references the events file for its pairs, so the retry reads the same
  // single copy commit itself promoted from.
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(452, value)(),
            RPCError::kRPCNone);
  auto retained = ctx.indexer_outbox_store().load_committed(13)();
  ASSERT_TRUE(retained.has_value()) << retained.error();
  expect_xdr_equal(*retained, committed);

  // The events file IS the record's payload now, so corrupting it corrupts the
  // record: a later retry fails closed instead of serving damaged pairs.
  corrupt_events_file(temp_dir(), closed.root_hash());
  EXPECT_NE(mempool_rpc.eventsched_commit_block(453, value)(),
            RPCError::kRPCNone);
  EXPECT_FALSE(ctx.indexer_outbox_store().load_committed(13)().has_value());
}

using EventRpcDeathTest = EventRpcTest;

TEST_F(EventRpcDeathTest, DuplicateCloseFailStops)
{
  // The scheduler closes each block exactly once, so a second close is a
  // scheduler bug rather than a retry to absorb. It must fail stop, not
  // quietly re-answer from durable state: silently tolerating it would hide
  // the caller's bug and let two different heights be attributed to one block.
  //
  // The whole sequence runs inside the death-test child: a fork with the
  // coroutine threadpool already spun up in the parent would not carry its
  // threads across.
  EXPECT_DEATH(
      {
        EventRPCContext ctx(temp_dir());
        EventMempoolRpc mempool_rpc(ctx);
        std::ignore = mempool_rpc.eventsched_open_block(46)();
        std::ignore = mempool_rpc.eventsched_close_block(46, 14)();
        std::ignore = mempool_rpc.eventsched_close_block(46, 14)();
      },
      "");
}

TEST_F(EventRpcTest, CommitPromotesEventsFileAfterContextRestart)
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
    ASSERT_EQ(mempool_rpc.eventsched_open_block(47)(), RPCError::kRPCNone);
    EventBlockResult closed = mempool_rpc.eventsched_close_block(47, 1)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    final_file = EventBlockContext::final_events_filename(temp_dir(),
                                                          closed.root_hash());
    value = make_consensus_value(1, closed.root_hash(), proposal_txs);
    value.prev_hash = sha256(genesis);
    // Leave the block closed and un-torn-down: a crash runs no destructors and
    // certainly does not abandon, and abandonment would mean a sibling
    // committed -- the opposite of the case being modelled.  Dropping the
    // context reclaims nothing, so the file is still there on restart.
    ASSERT_TRUE(std::filesystem::exists(final_file));
  }

  // This is the crash window the durable outbox exists for: consensus has
  // finalized the block and chuckd has persisted the ConsensusValue, but the
  // event service died before commit promoted it. On restart the block's
  // events must still be recoverable, or the indexer outbox has a permanent
  // hole. The events file is what recovers them -- promotion finds it by the
  // finalized ProposalHeader.events_hash, which is its name.
  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(47, value)().has_value());
  EXPECT_TRUE(std::filesystem::exists(final_file));
  auto committed = restarted.indexer_outbox_store().load_committed(1)();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  expect_xdr_equal(committed->consensus_value, value);
}

TEST_F(EventRpcTest, ReconciliationFailsWithoutCurrentEpochOrigin)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched_open_block(48)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(48, 16)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(16, closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(48, value)(),
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
      EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(card).outcome(),
                EventPrepareOutcome::EventPrepareNotReady);
    }
    EXPECT_FALSE(registration.event_registered(nullptr));
    auto origin = ctx.indexer_outbox_store().load_current_epoch_origin();
    ASSERT_TRUE(origin.has_value()) << origin.error();
    EXPECT_FALSE(origin->has_value());

    EventCard mismatched = card;
    mismatched.protocol = "hogp/not-this-binary";
    EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(mismatched).outcome(),
              EventPrepareOutcome::EventPrepareError);

    ConsensusValue genesis{};
    genesis.proposal_header.block_height = 0;
    registration.publish_event_canonical_tip(
        EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
    EventPrepareResult prepared =
        MempoolEventRpc::schedevent_prepare_event(card);
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
    ASSERT_EQ(MempoolEventRpc::schedevent_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);

    ASSERT_EQ(event_rpc.eventsched_open_block(kBlockOne)(), RPCError::kRPCNone);
    EventBlockResult closed = event_rpc.eventsched_close_block(kBlockOne, 1)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    block_one = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
    block_one.prev_hash = sha256(genesis);
    ASSERT_EQ(event_rpc.eventsched_commit_block(kBlockOne, block_one)(),
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
      MempoolEventRpc::schedevent_prepare_event(replacement);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  ASSERT_TRUE(
      restarted
          .reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                prepared.snapshot().tip.consensus_value)()
          .has_value());
  ASSERT_EQ(
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot()).outcome(),
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
      MempoolEventRpc::schedevent_prepare_event(first);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(
        ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                 prepared.snapshot().tip.consensus_value)()
            .has_value());
    ASSERT_EQ(MempoolEventRpc::schedevent_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);
  }

  registration.clear();
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
  EventCard replacement{.protocol = version::kHogdProtocol(),
                        .address_for_exec = "127.0.0.1:3",
                        .address_for_sched = "127.0.0.1:4"};
  prepared = MempoolEventRpc::schedevent_prepare_event(replacement);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(
      restarted
          .reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                prepared.snapshot().tip.consensus_value)()
          .has_value());
  EXPECT_EQ(
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot()).outcome(),
      EventActivationOutcome::EventActivationOK);
}

TEST_F(EventRpcTest, ChangedStartupTipReconcilesAgainBeforeActivation)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  EventPrepareResult prepared = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);

  EventRPCContext ctx(temp_dir());
  ASSERT_TRUE(
      ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                               prepared.snapshot().tip.consensus_value)()
          .has_value());

  constexpr BlockID kBlockOne = 591;
  EventMempoolRpc event_rpc(ctx);
  ASSERT_EQ(event_rpc.eventsched_open_block(kBlockOne)(), RPCError::kRPCNone);
  EventBlockResult closed = event_rpc.eventsched_close_block(kBlockOne, 1)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue block_one =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  block_one.prev_hash = sha256(genesis);
  ASSERT_EQ(event_rpc.eventsched_commit_block(kBlockOne, block_one)(),
            RPCError::kRPCNone);
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = kBlockOne, .consensus_value = block_one});

  EventActivationResult activation =
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot());
  ASSERT_EQ(activation.outcome(), EventActivationOutcome::EventActivationRetry);
  EXPECT_EQ(activation.current_tip().block_id, kBlockOne);

  prepared = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  ASSERT_TRUE(
      ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                               prepared.snapshot().tip.consensus_value)()
          .has_value());
  EXPECT_EQ(
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot()).outcome(),
      EventActivationOutcome::EventActivationOK);
  EXPECT_TRUE(registration.event_registered(nullptr));
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
      MempoolEventRpc::schedevent_prepare_event(first);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(
        ctx.reconcile_outbox_tip(prepared.snapshot().tip.block_id,
                                 prepared.snapshot().tip.consensus_value)()
            .has_value());
    ASSERT_EQ(MempoolEventRpc::schedevent_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);
  }

  registration.clear();
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});
  EventCard replacement{.protocol = version::kHogdProtocol(),
                        .address_for_exec = "127.0.0.1:3",
                        .address_for_sched = "127.0.0.1:4"};
  prepared = MempoolEventRpc::schedevent_prepare_event(replacement);
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
    ASSERT_EQ(MempoolEventRpc::schedevent_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);
    ASSERT_EQ(mempool_rpc.eventsched_open_block(kBlock)(), RPCError::kRPCNone);
    EventBlockResult closed = mempool_rpc.eventsched_close_block(kBlock, 1)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    height_one = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
    height_one.prev_hash = sha256(genesis);
    ASSERT_EQ(mempool_rpc.eventsched_commit_block(kBlock, height_one)(),
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
      ASSERT_EQ(mempool_rpc.eventsched_open_block(block_id)(),
                RPCError::kRPCNone);
      EventBlockResult closed =
          mempool_rpc.eventsched_close_block(block_id, height)();
      ASSERT_EQ(closed.error(), RPCError::kRPCNone);
      ConsensusValue value =
          make_consensus_value(height, closed.root_hash(), ProposalTxs{});
      value.prev_hash = sha256(previous);
      ASSERT_EQ(mempool_rpc.eventsched_commit_block(block_id, value)(),
                RPCError::kRPCNone);
      previous = std::move(value);
    }
  }

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(
      restarted.reconcile_outbox_tip(kSecondBlock, previous)().has_value());
}

TEST_F(EventRpcTest,
       ArchivalPrefixRestartInitializesCurrentOriginAndCommitsFirstBlock)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  auto archival_first = make_empty_committed(20, ConsensusValueHash{});
  auto archival_tip =
      make_empty_committed(21, sha256(archival_first.consensus_value));
  {
    EventRPCContext ctx(temp_dir());
    initialize_test_origin(ctx, archival_first.consensus_value);
    ASSERT_TRUE(ctx.indexer_outbox_store()
                    .publish_committed(archival_first)()
                    .has_value());
    ASSERT_TRUE(ctx.indexer_outbox_store()
                    .publish_committed(archival_tip)()
                    .has_value());
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
      MempoolEventRpc::schedevent_prepare_event(current_card);
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
    ASSERT_EQ(MempoolEventRpc::schedevent_activate_event(prepared.snapshot())
                  .outcome(),
              EventActivationOutcome::EventActivationOK);

    EventMempoolRpc mempool_rpc(restarted);
    ASSERT_EQ(mempool_rpc.eventsched_open_block(kCurrentBlock)(),
              RPCError::kRPCNone);
    EventBlockResult closed =
        mempool_rpc.eventsched_close_block(kCurrentBlock, 22)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    current_tip = make_consensus_value(22, closed.root_hash(), ProposalTxs{});
    current_tip.prev_hash = sha256(archival_tip.consensus_value);
    ASSERT_EQ(mempool_rpc.eventsched_commit_block(kCurrentBlock, current_tip)(),
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
  auto archival = make_empty_committed(20, ConsensusValueHash{});
  {
    EventRPCContext epoch_a(temp_dir());
    initialize_test_origin(epoch_a, archival.consensus_value);
    ASSERT_TRUE(epoch_a.indexer_outbox_store()
                    .publish_committed(archival)()
                    .has_value());
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

  auto current = make_empty_committed(21, sha256(archival.consensus_value));
  {
    EventRPCContext epoch_c(temp_dir());
    ASSERT_TRUE(epoch_c.reconcile_outbox_tip(740, archival.consensus_value)()
                    .has_value());
    ASSERT_TRUE(epoch_c.indexer_outbox_store()
                    .publish_committed(current)()
                    .has_value());
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
      ASSERT_EQ(mempool_rpc.eventsched_open_block(kBlock)(),
                RPCError::kRPCNone);
      EventBlockResult closed = mempool_rpc.eventsched_close_block(kBlock, 1)();
      ASSERT_EQ(closed.error(), RPCError::kRPCNone);
      committed = make_consensus_value(1, closed.root_hash(), ProposalTxs{});
      committed.prev_hash = boundary_hash;
      ASSERT_EQ(mempool_rpc.eventsched_commit_block(kBlock, committed)(),
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

TEST_F(EventRpcTest, MultipleEmptyArchivalEpochsPreserveCanonicalBoundary)
{
  auto archival = make_empty_committed(20, ConsensusValueHash{});
  {
    EventRPCContext epoch_a(temp_dir());
    initialize_test_origin(epoch_a, archival.consensus_value);
    ASSERT_TRUE(epoch_a.indexer_outbox_store()
                    .publish_committed(archival)()
                    .has_value());
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
  auto archival = make_empty_committed(20, ConsensusValueHash{});
  {
    EventRPCContext epoch_a(temp_dir());
    initialize_test_origin(epoch_a, archival.consensus_value);
    ASSERT_TRUE(epoch_a.indexer_outbox_store()
                    .publish_committed(archival)()
                    .has_value());
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

TEST_F(EventRpcTest, ReconciliationRejectsArchivalProtocolReentry)
{
  auto first = make_empty_committed(20, ConsensusValueHash{});
  auto second = make_empty_committed(21, sha256(first.consensus_value));
  auto third = make_empty_committed(22, sha256(second.consensus_value));
  {
    EventRPCContext ctx(temp_dir());
    initialize_test_origin(ctx, first.consensus_value);
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(first)().has_value());
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(second)().has_value());
    ASSERT_TRUE(
        ctx.indexer_outbox_store().publish_committed(third)().has_value());
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

  ASSERT_EQ(mempool_rpc.eventsched_open_block(320)(), RPCError::kRPCNone);
  EventBlockResult archival_closed =
      mempool_rpc.eventsched_close_block(320, 20)();
  ASSERT_EQ(archival_closed.error(), RPCError::kRPCNone);
  ConsensusValue archival =
      make_consensus_value(20, archival_closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, archival);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(320, archival)(),
            RPCError::kRPCNone);

  ASSERT_EQ(mempool_rpc.eventsched_open_block(321)(), RPCError::kRPCNone);
  EventBlockResult current_closed =
      mempool_rpc.eventsched_close_block(321, 21)();
  ASSERT_EQ(current_closed.error(), RPCError::kRPCNone);
  ConsensusValue current =
      make_consensus_value(21, current_closed.root_hash(), proposal_txs);
  current.prev_hash = sha256(archival);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(321, current)(),
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

  ASSERT_EQ(mempool_rpc.eventsched_open_block(322)(), RPCError::kRPCNone);
  EventBlockResult archival_closed =
      mempool_rpc.eventsched_close_block(322, 20)();
  ASSERT_EQ(archival_closed.error(), RPCError::kRPCNone);
  ConsensusValue archival =
      make_consensus_value(20, archival_closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, archival);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(322, archival)(),
            RPCError::kRPCNone);

  ASSERT_EQ(mempool_rpc.eventsched_open_block(323)(), RPCError::kRPCNone);
  EventBlockResult current_closed =
      mempool_rpc.eventsched_close_block(323, 21)();
  ASSERT_EQ(current_closed.error(), RPCError::kRPCNone);
  ConsensusValue current =
      make_consensus_value(21, current_closed.root_hash(), proposal_txs);
  current.prev_hash = sha256(archival);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(323, current)(),
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
                  .initialize_current_epoch_origin(21, sha256(tip))()
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
                    .initialize_current_epoch_origin(20, sha256(tip))()
                    .has_value());
    EXPECT_FALSE(ctx.reconcile_outbox_tip(200, tip)().has_value());
  }
  {
    TmpDir dir;
    EventRPCContext ctx(dir.directory());
    ConsensusValueHash wrong_hash = sha256(tip);
    wrong_hash[0] ^= 0xff;
    ASSERT_TRUE(ctx.indexer_outbox_store()
                    .initialize_current_epoch_origin(21, wrong_hash)()
                    .has_value());
    EXPECT_FALSE(ctx.reconcile_outbox_tip(200, tip)().has_value());
  }
}

TEST_F(EventRpcTest, ReconciliationValidatesCompleteMultiRecordArchivalEpoch)
{
  EventRPCContext ctx(temp_dir());
  auto first = make_empty_committed(20, ConsensusValueHash{});
  auto second = make_empty_committed(21, sha256(first.consensus_value));
  auto current = make_empty_committed(22, sha256(second.consensus_value));
  initialize_test_origin(ctx, first.consensus_value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(first)().has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(second)().has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(current)().has_value());
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
  auto first = make_empty_committed(20, ConsensusValueHash{});
  ConsensusValueHash disconnected{};
  disconnected[0] = 0xff;
  auto second = make_empty_committed(21, sha256(first.consensus_value));
  auto current = make_empty_committed(22, sha256(second.consensus_value));
  initialize_test_origin(ctx, first.consensus_value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(first)().has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(second)().has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(current)().has_value());
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
  auto first = make_empty_committed(20, ConsensusValueHash{});
  auto second = make_empty_committed(21, sha256(first.consensus_value));
  auto current = make_empty_committed(22, sha256(second.consensus_value));
  initialize_test_origin(ctx, first.consensus_value);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(first)().has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(second)().has_value());
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(current)().has_value());
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

TEST_F(EventRpcTest, ReconciliationRejectsCorruptCurrentEpochPayload)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched_open_block(214)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(214, 21)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(21, closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(214, value)(),
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

  ASSERT_EQ(mempool_rpc.eventsched_open_block(211)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(211, 21)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue ahead =
      make_consensus_value(21, closed.root_hash(), proposal_txs);
  ahead.prev_hash[0] = 0xff;
  initialize_test_origin(ctx, ahead);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(211, ahead)(),
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

  ASSERT_EQ(mempool_rpc.eventsched_open_block(212)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(212, 21)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue first =
      make_consensus_value(21, closed.root_hash(), proposal_txs);
  first.prev_hash = sha256(scheduler_tip);
  initialize_test_origin(ctx, first);
  ASSERT_EQ(mempool_rpc.eventsched_commit_block(212, first)(),
            RPCError::kRPCNone);
  auto first_record = ctx.indexer_outbox_store().load_committed(21)();
  ASSERT_TRUE(first_record.has_value()) << first_record.error();
  IndexerOutboxCommittedPayload second = *first_record;
  second.consensus_value.proposal_header.block_height = 22;
  second.consensus_value.prev_hash = sha256(first);
  ASSERT_TRUE(
      ctx.indexer_outbox_store().publish_committed(second)().has_value());

  EXPECT_FALSE(ctx.reconcile_outbox_tip(200, scheduler_tip)().has_value());
}

// Regression for #1960: closing may retain a durable candidate for recovery,
// but indexers must not be able to pull it unless it is committed.
TEST_F(EventRpcTest, CloseDoesNotMakeEventsPullableBeforeCommit)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ProposedTransaction body = make_body(1);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, make_event(body, 1),
                                              body)(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1, 1)();
  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  const std::string events_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());
  // Closing makes the block durable but must not make it visible: commit is
  // the publication barrier, so nothing is pullable until then.
  EXPECT_TRUE(std::filesystem::exists(events_file));
  EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(1).value());
  EXPECT_EQ(ctx.pull_indexer_next(pull_request())().outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp);

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
  // Abandonment means a sibling committed, so the block lost and its events
  // file is reclaimed with its last claim.
  EXPECT_FALSE(std::filesystem::exists(events_file));
  EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(1).value());
  EXPECT_EQ(ctx.pull_indexer_next(pull_request())().outcome(),
            EventIndexerPullOutcome::EventIndexerPullCaughtUp);
}

TEST_F(EventRpcTest, RecoveryPicksCanonicalEventsFileAmongCompetingBlocks)
{
  // Two blocks close at the same height with different nondeterministic data,
  // so they have different event roots and therefore different events files.
  // After a restart the scheduler's BlockID is regenerated, so recovery cannot
  // match on it. It matches the finalized ProposalHeader.events_hash, which is
  // exactly the canonical file's name.
  ProposedTransaction canonical_body = make_body(210);
  ProposedTransaction competing_body = canonical_body;
  competing_body.nondeterministic_proposal_data.nondeterministic_bits[0] ^=
      0xff;
  ProposalTxs canonical_txs = make_proposal_txs({canonical_body});
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue canonical_value;
  SHA256Hash competing_root;

  {
    EventRPCContext ctx(temp_dir());
    EventExecRpc exec_rpc(ctx);
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    auto close_one = [&](BlockID block_id, const ProposedTransaction &body) {
      EXPECT_EQ(mempool_rpc.eventsched_open_block(block_id)(),
                RPCError::kRPCNone);
      EXPECT_EQ(exec_rpc.eventexecutor_open_block(block_id)(),
                RPCError::kRPCNone);
      EXPECT_EQ(exec_rpc.eventexecutor_emit_event(block_id, 1,
                                                  make_event(body, 0x21),
                                                  body)(),
                RPCError::kRPCNone);
      EXPECT_EQ(exec_rpc.eventexecutor_close_block(block_id)(),
                RPCError::kRPCNone);
      EventBlockResult closed =
          mempool_rpc.eventsched_close_block(block_id, 1)();
      EXPECT_EQ(closed.error(), RPCError::kRPCNone);
      return closed.root_hash();
    };
    competing_root = close_one(209, competing_body);
    const SHA256Hash canonical_root = close_one(210, canonical_body);
    ASSERT_NE(competing_root, canonical_root);
    canonical_value = make_consensus_value(1, canonical_root, canonical_txs);
    canonical_value.prev_hash = sha256(genesis);
  }

  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(
      restarted.reconcile_outbox_tip(310, canonical_value)().has_value());

  auto committed = restarted.indexer_outbox_store().load_committed(1)();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  expect_xdr_equal(committed->consensus_value, canonical_value);
  ASSERT_EQ(committed->tx_bodies.size(), 1);
  expect_xdr_equal(committed->tx_bodies[0], canonical_body);
  // The losing block's events file is inert, not promoted.
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir(), competing_root)));
}

TEST_F(EventRpcTest, IndexerPullsBlockRecoveredAfterCrashBetweenCloseAndCommit)
{
  // The data loss this whole durable path exists to prevent: a block closes,
  // consensus finalizes it and chuckd persists the ConsensusValue, and the
  // event service dies before commit promotes it. After restart the indexer
  // must still be able to pull that block's events and bodies.
  ProposedTransaction body = make_body(77);
  TxEventLog event = make_event(body, 0x77);
  ProposalTxs proposal_txs = make_proposal_txs({body});
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue value;

  {
    EventRPCContext ctx(temp_dir());
    EventExecRpc exec_rpc(ctx);
    EventMempoolRpc mempool_rpc(ctx);
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    ASSERT_EQ(mempool_rpc.eventsched_open_block(77)(), RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_open_block(77)(), RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_emit_event(77, 1, event, body)(),
              RPCError::kRPCNone);
    ASSERT_EQ(exec_rpc.eventexecutor_close_block(77)(), RPCError::kRPCNone);
    EventBlockResult closed = mempool_rpc.eventsched_close_block(77, 1)();
    ASSERT_EQ(closed.error(), RPCError::kRPCNone);
    value = make_consensus_value(1, closed.root_hash(), proposal_txs);
    value.prev_hash = sha256(genesis);
    // Deliberately no commit: this is the crash.
  }

  EventRPCContext restarted(temp_dir());
  ASSERT_TRUE(restarted.reconcile_outbox_tip(177, value)().has_value());

  auto pulled = restarted.pull_indexer_next(pull_request())();
  ASSERT_EQ(pulled.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  const auto &payload = pulled.pulled_block().block.payload;
  expect_xdr_equal(payload.consensus_value, value);
  ASSERT_EQ(payload.event_logs.size(), 1);
  ASSERT_EQ(payload.tx_bodies.size(), 1);
  expect_xdr_equal(payload.event_logs[0], event);
  expect_xdr_equal(payload.tx_bodies[0], body);
}

TEST_F(EventRpcTest, ReconciliationDoesNotRepromoteArchivalCommittedTip)
{
  // An events file can outlive the commit of its height: a crash leaves one
  // behind with no live claim on it, and a file shared with another block
  // survives until that block settles.  Its presence must never be read as
  // "this height still needs promoting".
  //
  // A committed record tagged with an archival protocol is not the
  // current-protocol tip, so a promotion gate keyed on that would fire here and
  // try to commit the height a second time off its retained events file.
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue block;
  std::string events_file;
  {
    // Scoped: the outbox directory takes an exclusive lock, so the first
    // context has to go away before the restarted one can open it.
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    block = commit_empty_block(ctx, 861, 1, sha256(genesis));
    events_file = EventBlockContext::final_events_filename(
        temp_dir(), block.proposal_header.events_hash);
  }

  // Put the file back to stand in for one a crash left behind at this root.
  {
    std::ofstream leftover(events_file, std::ios::binary);
  }
  ASSERT_TRUE(std::filesystem::exists(events_file));

  rewrite_outbox_protocol(temp_dir(), IndexerOutboxStore::committed_filename(1),
                          "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(temp_dir()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin_header(temp_dir(), "hogp/archival", 1, sha256(genesis), 0);

  EventRPCContext restarted(temp_dir());
  EXPECT_TRUE(restarted.reconcile_outbox_tip(1, block)().has_value());
  EXPECT_TRUE(std::filesystem::exists(events_file));
}

TEST_F(EventRpcTest, RestartSeedsClaimsForCommittedRecordsEventsFiles)
{
  // A committed record published by an earlier run references its events file,
  // but in-memory claims die with the process. Reconciliation must re-seed
  // them: otherwise a later live block that shares the root -- any empty block
  // does -- would unlink the record's payload when it is abandoned, and the
  // record would stop loading.
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ConsensusValue one;
  std::string events_file;
  {
    EventRPCContext ctx(temp_dir());
    ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());
    one = commit_empty_block(ctx, 55, 1, sha256(genesis));
    events_file = EventBlockContext::final_events_filename(
        temp_dir(), one.proposal_header.events_hash);
    ASSERT_TRUE(std::filesystem::exists(events_file));
  }

  EventRPCContext restarted(temp_dir());
  EventMempoolRpc rpc(restarted);
  ASSERT_TRUE(restarted.reconcile_outbox_tip(1, one)().has_value());

  // A fresh empty block at the next height closes to the same root, loses its
  // nomination, and is abandoned.
  ASSERT_EQ(rpc.eventsched_open_block(56)(), RPCError::kRPCNone);
  EventBlockResult closed = rpc.eventsched_close_block(56, 2)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ASSERT_EQ(EventBlockContext::final_events_filename(temp_dir(),
                                                     closed.root_hash()),
            events_file);
  ASSERT_EQ(rpc.eventsched_abandon_block(56)(), RPCError::kRPCNone);

  // The record's claim, seeded at reconciliation, kept the file alive.
  EXPECT_TRUE(std::filesystem::exists(events_file));
  EXPECT_TRUE(restarted.indexer_outbox_store().load_committed(1)().has_value());
}

TEST_F(EventRpcTest, StaleTempFileFromAReusedBlockIdIsDiscarded)
{
  // Block ids are process-local, so a crash can leave <block_id>.events_temp
  // behind for a name a later run reuses. Opening it without truncating would
  // write the new block from offset zero and publish whatever stale tail was
  // longer -- for an empty block, the entire stale file. The published events
  // file would then fail root validation at commit.
  constexpr BlockID kReusedBlockId = 71;
  {
    ProposedTransaction stale_body = make_body(71);
    x23::Buf stale;
    x23::serialize_to(&stale, make_event(stale_body, 71));
    x23::serialize_to(&stale, stale_body);
    std::ofstream leftover(
        EventBlockContext::temp_events_filename(temp_dir(), kReusedBlockId),
        std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(leftover.write(reinterpret_cast<const char *>(stale.data()),
                               static_cast<std::streamsize>(stale.size())));
  }

  EventRPCContext ctx(temp_dir());
  EventMempoolRpc rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ASSERT_EQ(rpc.eventsched_open_block(kReusedBlockId)(), RPCError::kRPCNone);
  EventBlockResult closed = rpc.eventsched_close_block(kReusedBlockId, 1)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);

  // The block is empty, so its events file must be too -- not the stale pair.
  auto pairs = read_final_events_file(temp_dir(), closed.root_hash());
  ASSERT_TRUE(pairs.has_value()) << pairs.error();
  EXPECT_TRUE(pairs->event_logs.empty());
  EXPECT_TRUE(pairs->tx_bodies.empty());

  ConsensusValue value =
      make_consensus_value(1, closed.root_hash(), ProposalTxs{});
  value.prev_hash = sha256(genesis);
  EXPECT_EQ(rpc.eventsched_commit_block(kReusedBlockId, value)(),
            RPCError::kRPCNone);
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(1).value());
}

TEST_F(EventRpcTest, EmptyBlocksShareOneEventsFileAcrossHeights)
{
  // Every empty block has the same event root -- the empty trie's -- so they
  // all publish under one content-addressed name.  That name is therefore not
  // one block's property: it has to survive until the last block holding it
  // has settled, whatever order they close and commit in.
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ASSERT_EQ(rpc.eventsched_open_block(51)(), RPCError::kRPCNone);
  EventBlockResult first = rpc.eventsched_close_block(51, 1)();
  ASSERT_EQ(first.error(), RPCError::kRPCNone);
  ASSERT_EQ(rpc.eventsched_open_block(52)(), RPCError::kRPCNone);
  EventBlockResult second = rpc.eventsched_close_block(52, 2)();
  ASSERT_EQ(second.error(), RPCError::kRPCNone);

  EXPECT_EQ(first.root_hash(), second.root_hash());
  const std::string events_file =
      EventBlockContext::final_events_filename(temp_dir(), first.root_hash());
  ASSERT_TRUE(std::filesystem::exists(events_file));

  ConsensusValue one =
      make_consensus_value(1, first.root_hash(), ProposalTxs{});
  one.prev_hash = sha256(genesis);
  ASSERT_EQ(rpc.eventsched_commit_block(51, one)(), RPCError::kRPCNone);
  // Height 2 is closed and still has to read this file at commit, so
  // committing height 1 must not take it away.
  EXPECT_TRUE(std::filesystem::exists(events_file));

  ConsensusValue two =
      make_consensus_value(2, second.root_hash(), ProposalTxs{});
  two.prev_hash = sha256(one);
  ASSERT_EQ(rpc.eventsched_commit_block(52, two)(), RPCError::kRPCNone);
  // Both committed records reference this one file as their payload, so it
  // outlives both block contexts.
  EXPECT_TRUE(std::filesystem::exists(events_file));

  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(1).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(2).value());
  EXPECT_TRUE(ctx.indexer_outbox_store().load_committed(1)().has_value());
  EXPECT_TRUE(ctx.indexer_outbox_store().load_committed(2)().has_value());
}

TEST_F(EventRpcTest, AbandoningOneEmptyBlockKeepsTheSharedFileForAnother)
{
  // Two empty nominations race at height 1 and an empty block at height 2 is
  // already closed behind them.  All three are the same file.  When the loser
  // at height 1 is abandoned, reclaiming "its" file would leave height 2's
  // commit reading a file that is no longer there -- a fail-closed commit on
  // an ordinary idle chain.
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc rpc(ctx);
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  ASSERT_TRUE(ctx.reconcile_outbox_tip(0, genesis)().has_value());

  ASSERT_EQ(rpc.eventsched_open_block(61)(), RPCError::kRPCNone);
  EventBlockResult loser = rpc.eventsched_close_block(61, 1)();
  ASSERT_EQ(loser.error(), RPCError::kRPCNone);
  ASSERT_EQ(rpc.eventsched_open_block(62)(), RPCError::kRPCNone);
  EventBlockResult winner = rpc.eventsched_close_block(62, 1)();
  ASSERT_EQ(winner.error(), RPCError::kRPCNone);
  ASSERT_EQ(rpc.eventsched_open_block(63)(), RPCError::kRPCNone);
  EventBlockResult next_height = rpc.eventsched_close_block(63, 2)();
  ASSERT_EQ(next_height.error(), RPCError::kRPCNone);

  const std::string events_file =
      EventBlockContext::final_events_filename(temp_dir(), loser.root_hash());
  ASSERT_EQ(loser.root_hash(), winner.root_hash());
  ASSERT_EQ(loser.root_hash(), next_height.root_hash());
  ASSERT_TRUE(std::filesystem::exists(events_file));

  ConsensusValue one =
      make_consensus_value(1, winner.root_hash(), ProposalTxs{});
  one.prev_hash = sha256(genesis);
  ASSERT_EQ(rpc.eventsched_commit_block(62, one)(), RPCError::kRPCNone);

  // The sibling committing is exactly why the loser is abandoned.
  ASSERT_EQ(rpc.eventsched_abandon_block(61)(), RPCError::kRPCNone);
  EXPECT_TRUE(std::filesystem::exists(events_file));

  ConsensusValue two =
      make_consensus_value(2, next_height.root_hash(), ProposalTxs{});
  two.prev_hash = sha256(one);
  EXPECT_EQ(rpc.eventsched_commit_block(63, two)(), RPCError::kRPCNone);
  EXPECT_TRUE(ctx.indexer_outbox_store().has_committed(2).value());
  // The two committed records keep the file alive as their payload.
  EXPECT_TRUE(std::filesystem::exists(events_file));
  EXPECT_TRUE(ctx.indexer_outbox_store().load_committed(2)().has_value());
}

TEST_F(EventRpcTest, CommitFailsClosedWhenEventsFileIsMissing)
{
  EventRPCContext ctx(temp_dir());
  EventMempoolRpc mempool_rpc(ctx);
  ProposalTxs proposal_txs;

  ASSERT_EQ(mempool_rpc.eventsched_open_block(88)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(88, 1)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(1, closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  ASSERT_TRUE(std::filesystem::remove(
      EventBlockContext::final_events_filename(temp_dir(),
                                               closed.root_hash())));

  // Losing the events file must fail the commit. Publishing an empty or
  // partial record instead would put a silent hole in the indexer's history.
  EXPECT_NE(mempool_rpc.eventsched_commit_block(88, value)(),
            RPCError::kRPCNone);
  EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(1).value());
}

TEST_F(EventRpcTest, CommitFailsClosedWhenEventsFileIsCorrupt)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  ProposedTransaction body = make_body(99);
  ProposalTxs proposal_txs = make_proposal_txs({body});

  ASSERT_EQ(mempool_rpc.eventsched_open_block(99)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_open_block(99)(), RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_emit_event(99, 1, make_event(body, 0x99),
                                              body)(),
            RPCError::kRPCNone);
  ASSERT_EQ(exec_rpc.eventexecutor_close_block(99)(), RPCError::kRPCNone);
  EventBlockResult closed = mempool_rpc.eventsched_close_block(99, 1)();
  ASSERT_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(1, closed.root_hash(), proposal_txs);
  initialize_test_origin(ctx, value);
  corrupt_events_file(temp_dir(), closed.root_hash());

  // A truncated events file must fail the commit rather than promote whatever
  // pairs happened to parse.
  EXPECT_NE(mempool_rpc.eventsched_commit_block(99, value)(),
            RPCError::kRPCNone);
  EXPECT_FALSE(ctx.indexer_outbox_store().has_committed(1).value());
}

} // namespace hogd
