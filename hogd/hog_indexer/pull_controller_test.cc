#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono> // IWYU pragma: keep
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_conflict.hh"
#include "event.hh"
#include "event_outbox_internal.hh"
#include "event_rpc.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/hog_event/rpc_context.h"
#include "hogd/hog_event/rpc_from_exec.h"
#include "hogd/hog_event/rpc_from_indexer.h"
#include "hogd/hog_event/rpc_from_sched.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/hog_indexer/pull_controller.h"
#include "hogd/hog_indexer/recent_tx_failure_cache.h"
#include "hogd/version.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "network.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {
namespace {

template<typename T>
void
expect_xdr_equal(const T &lhs, const T &rhs)
{
  EXPECT_EQ(x23::serialize(lhs), x23::serialize(rhs));
}

ProposedTransaction
make_body(std::uint64_t nonce)
{
  ProposedTransaction body{};
  body.signed_tx.tx.uniquifier.block_expiry_height = 1000 + nonce;
  body.signed_tx.tx.uniquifier.nonce = nonce;
  body.signed_tx.tx.invocation.gas_limit = 1'000'000 + nonce;
  body.nondeterministic_proposal_data.nondeterministic_bits.push_back(
      static_cast<std::uint8_t>(nonce));
  body.nondeterministic_proposal_data.nondeterministic_bit_count = 8;
  return body;
}

TxEventLog
make_event(const ProposedTransaction &body, std::uint8_t marker)
{
  TxEventLog event{};
  event.tx_hash = sha256(body.signed_tx.tx);
  TxEmittedEvent emitted{};
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

ConsensusValue
commit_block(EventRPCContext &ctx, BlockID block_id, BlockHeight height,
             ConsensusValueHash prev_hash,
             std::optional<ProposedTransaction> body = std::nullopt)
{
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);
  CHECK_EQ(mempool_rpc.eventsched_open_block(block_id)(), RPCError::kRPCNone);

  ProposedTransactionBatch bodies;
  if (body) {
    CHECK_EQ(exec_rpc.eventexecutor_open_block(block_id)(), RPCError::kRPCNone);
    CHECK_EQ(exec_rpc.eventexecutor_emit_event(block_id, 1,
                                               make_event(*body, 0xA1),
                                               *body)(),
             RPCError::kRPCNone);
    CHECK_EQ(exec_rpc.eventexecutor_close_block(block_id)(),
             RPCError::kRPCNone);
    bodies.push_back(*body);
  }
  ProposalTxs proposal_txs = make_proposal_txs(bodies);
  EventBlockResult closed =
      mempool_rpc.eventsched_close_block(block_id, height)();
  CHECK_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value =
      make_consensus_value(height, closed.root_hash(), proposal_txs);
  value.prev_hash = std::move(prev_hash);
  CHECK_EQ(mempool_rpc.eventsched_commit_block(block_id, value)(),
           RPCError::kRPCNone);
  return value;
}

void
rewrite_outbox_protocol(std::string_view directory, BlockHeight height,
                        std::string_view protocol)
{
  const std::filesystem::path path =
      std::filesystem::path(directory) / "indexer-outbox" /
      IndexerOutboxStore::committed_filename(height);
  auto bytes = x23::readfile<x23::Buf>(path.string());
  ASSERT_TRUE(bytes.has_value());
  std::uint32_t header_size = 0;
  x23::deserialize(&header_size, x23::BufView(bytes->data(), std::size_t{4}));
  IndexerOutboxRecordHeader header;
  x23::deserialize(&header, x23::BufView(bytes->data() + 4, header_size));
  header.protocol = ProtocolCommit(protocol.data(), protocol.size());
  x23::Buf header_bytes = x23::serialize(header);
  x23::Buf rewritten =
      x23::serialize(static_cast<std::uint32_t>(header_bytes.size()));
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
write_epoch_origin(std::string_view directory, std::string_view protocol,
                   BlockHeight first_height, ConsensusValueHash first_prev_hash,
                   std::uint64_t epoch_sequence)
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
  x23::Buf bytes =
      x23::serialize(static_cast<std::uint32_t>(header_bytes.size()));
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

template<typename Predicate>
bool
wait_for_condition(Predicate &&predicate, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

class CaughtUpPullRpc {
public:
  using rpc_service = EventIndexer1;

  CaughtUpPullRpc()
  {
    origin_.protocol = version::kHogdProtocol();
    origin_.first_height = IndexDB::kFirstOutboxBlockHeight;
    std::ranges::fill(origin_.first_prev_hash, 0x42);
    ProposalTxs proposal{};
    ConsensusValue value{};
    value.prev_hash = origin_.first_prev_hash;
    value.proposal_header.block_height = origin_.first_height;
    value.proposal_header.block_timepoint = 1000;
    value.proposal_header.events_hash =
        compute_indexer_outbox_event_root({}, {});
    auto txs_root = compute_indexer_proposal_txs_root(proposal)();
    CHECK(txs_root.has_value());
    value.proposal_header.txs_hash = *txs_root;
    value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal);
    IndexerCommittedBlockPayload payload{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = std::move(value),
    };
    auto block = make_indexer_committed_block(payload)();
    CHECK(block.has_value());
    block_ = std::move(*block);
    std::ranges::fill(notice_.tx_hash, 0x51);
    notice_.diagnostic.attempt_height = 0;
    notice_.diagnostic.conflict.code = DbConflictCode::kDbConflictValueMismatch;
  }

  x23::Async<EventIndexerPullResult>
  event_indexer_pull_next(EventIndexerPullRequest request)
  {
    {
      std::lock_guard lock(mutex_);
      requests_.push_back(std::move(request));
    }
    const std::size_t call = calls_.fetch_add(1, std::memory_order_relaxed);
    if (call == 0) {
      co_return EventIndexerPullResult(
          EventIndexerPullOutcome::EventIndexerPullNotReady);
    }
    if (call == 1) {
      EventIndexerPullResult result(
          EventIndexerPullOutcome::EventIndexerPullBlock);
      result.pulled_block().origin = origin_;
      result.pulled_block().block = block_;
      co_return result;
    }
    EventIndexerPullResult result(
        EventIndexerPullOutcome::EventIndexerPullCaughtUp);
    result.origin() = origin_;
    co_return result;
  }

  x23::Async<IndexerTxFailureNoticeBatch> event_indexer_pull_tx_failures()
  {
    IndexerTxFailureNoticeBatch notices;
    if (!notice_returned_.exchange(true, std::memory_order_relaxed)) {
      notices.push_back(notice_);
    }
    co_return notices;
  }

  std::vector<EventIndexerPullRequest> requests() const
  {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  const IndexerOutboxOrigin &origin() const { return origin_; }
  const TransactionHash &failure_hash() const { return notice_.tx_hash; }

private:
  IndexerOutboxOrigin origin_{};
  IndexerCommittedBlock block_{};
  IndexerTxFailureNotice notice_{};
  std::atomic<std::size_t> calls_ = 0;
  std::atomic<bool> notice_returned_ = false;
  mutable std::mutex mutex_;
  std::vector<EventIndexerPullRequest> requests_;
};

static_assert(is_EventIndexer1<CaughtUpPullRpc *, x23::MaybeAsync>);

class BlockingPullRpc {
public:
  enum class Mode { kAlwaysBlock, kBlockFirst };
  using rpc_service = EventIndexer1;

  explicit BlockingPullRpc(Mode mode) : mode_(mode)
  {
    origin_.protocol = version::kHogdProtocol();
    origin_.first_height = IndexDB::kFirstOutboxBlockHeight;
    std::ranges::fill(origin_.first_prev_hash, 0x42);
  }

  x23::Async<EventIndexerPullResult>
  event_indexer_pull_next(EventIndexerPullRequest /*unused*/)
  {
    std::size_t call;
    {
      std::lock_guard lock(mutex_);
      call = calls_++;
    }
    calls_wake_.notify_all();
    if (mode_ == Mode::kAlwaysBlock || call == 0) {
      while (!released_.load()) {
        co_await released_.wait(false);
      }
    }
    EventIndexerPullResult result(
        EventIndexerPullOutcome::EventIndexerPullCaughtUp);
    result.origin() = origin_;
    co_return result;
  }

  static IndexerTxFailureNoticeBatch event_indexer_pull_tx_failures()
  {
    return {};
  }

  bool wait_for_calls(std::size_t count, std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return calls_wake_.wait_for(lock, timeout, [&] { return calls_ >= count; });
  }

  void release(x23::ThreadPool *pool)
  {
    released_.store(true);
    released_.broadcast(pool);
  }

private:
  Mode mode_;
  IndexerOutboxOrigin origin_{};
  std::mutex mutex_;
  std::condition_variable calls_wake_;
  std::size_t calls_ = 0;
  x23::CoroFutex<bool> released_ = false;
};

static_assert(is_EventIndexer1<BlockingPullRpc *, x23::MaybeAsync>);

TEST(IndexerPullController, RetriesAppliesBlockAndConfirmsCurrentProgress)
{
  TmpDir indexer_dir;
  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  RpcServer<CaughtUpPullRpc> event_server;
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  IndexerPullControllerOptions options;
  options.retry_delay = std::chrono::milliseconds(1);
  options.caught_up_poll_delay = std::chrono::seconds(1);

  IndexerPullController controller(
      std::string(x23::HostPort{"127.0.0.1", event_port}.show()), *db, failures,
      options);
  controller.wait_until_caught_up();

  const std::vector<EventIndexerPullRequest> requests =
      event_server.impl()->requests();
  ASSERT_GE(requests.size(), 3UL);
  EXPECT_EQ(std::string_view(requests[0].protocol), version::kHogdProtocol());
  EXPECT_EQ(requests[0].progress.state(),
            IndexerProgressState::IndexerProgressUninitialized);
  EXPECT_EQ(requests[1].progress.state(),
            IndexerProgressState::IndexerProgressUninitialized);
  ASSERT_EQ(requests[2].progress.state(),
            IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(requests[2].progress.initialized().origin,
            event_server.impl()->origin());
  EXPECT_EQ(requests[2].progress.initialized().contiguous_height, 1);
  EXPECT_EQ(db->durable_progress(), requests[2].progress);

  auto diagnostic = failures.lookup(event_server.impl()->failure_hash());
  ASSERT_TRUE(diagnostic.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
  EXPECT_EQ(diagnostic.value().conflict.code,
            DbConflictCode::kDbConflictValueMismatch);
}

TEST(IndexerPullController,
     RealEventServerReplaysArchivedAndCurrentEpochsAndResumesAfterRestart)
{
  TmpDir event_dir;
  TmpDir indexer_dir;
  const std::filesystem::path indexer_path =
      std::filesystem::path(indexer_dir.directory()) / "indexer-db";
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  const ProposedTransaction archival_body = make_body(7);

  ConsensusValue archival;
  {
    EventRPCContext event(event_dir.directory());
    ASSERT_TRUE(event.reconcile_outbox_tip(0, genesis)().has_value());
    archival = commit_block(event, 1, 1, sha256(genesis), archival_body);
  }
  rewrite_outbox_protocol(event_dir.directory(), 1, "hogp/archival");
  ASSERT_TRUE(std::filesystem::remove(
      std::filesystem::path(event_dir.directory()) / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));
  write_epoch_origin(event_dir.directory(), "hogp/archival", 1, sha256(genesis),
                     0);

  ConsensusValue current_two;
  {
    EventRPCContext event(event_dir.directory());
    ASSERT_TRUE(event.reconcile_outbox_tip(1, archival)().has_value());
    current_two = commit_block(event, 2, 2, sha256(archival));
  }

  EventRPCContext event(event_dir.directory());
  ASSERT_TRUE(event.reconcile_outbox_tip(2, current_two)().has_value());
  EventIndexerServer event_server(event);
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  const std::string event_addr =
      std::string(x23::HostPort{"127.0.0.1", event_port}.show());
  IndexerPullControllerOptions options;
  options.retry_delay = std::chrono::milliseconds(1);
  options.caught_up_poll_delay = std::chrono::milliseconds(1);

  {
    auto db = IndexDB::make_new(indexer_path);
    ASSERT_TRUE(db.has_value()) << db.error();
    RecentTxFailureCache failures;
    IndexerPullController controller(event_addr, *db, failures, options);
    controller.wait_until_caught_up();

    const IndexerProgress progress = db->durable_progress();
    ASSERT_EQ(progress.state(),
              IndexerProgressState::IndexerProgressInitialized);
    EXPECT_EQ(progress.initialized().contiguous_height, 2);
    EXPECT_EQ(progress.initialized().origin.epoch_sequence, 1UL);
    EXPECT_EQ(std::string_view(progress.initialized().origin.protocol),
              version::kHogdProtocol());
    auto record = db->get_tx_record(sha256(archival_body.signed_tx.tx));
    ASSERT_TRUE(record.has_value()) << record.error();
    EXPECT_EQ(record->height, 1);
    expect_xdr_equal(record->tx, archival_body);
    EXPECT_EQ(record->events.tx_hash, sha256(archival_body.signed_tx.tx));
    ASSERT_EQ(record->events.events.size(), 1UL);
    expect_xdr_equal(record->events.events.front(),
                     make_event(archival_body, 0xA1).events.front());
  }

  ConsensusValue current_three = commit_block(event, 3, 3, sha256(current_two));
  auto db = IndexDB::open(indexer_path);
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  IndexerPullController controller(event_addr, *db, failures, options);
  controller.wait_until_caught_up();
  const IndexerProgress progress = db->durable_progress();
  ASSERT_EQ(progress.state(), IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(progress.initialized().contiguous_height, 3);
  EXPECT_EQ(progress.initialized().last_consensus_hash, sha256(current_three));
  auto record = db->get_tx_record(sha256(archival_body.signed_tx.tx));
  ASSERT_TRUE(record.has_value()) << record.error();
  EXPECT_EQ(record->height, 1);
}

// The deployed topology in one process: the indexer connects to a freshly
// reconciled event server and catches up at the genesis epoch boundary before
// any block exists, and blocks then commit while it sits in its caught-up
// poll loop. Every other test here replays an outbox that was committed
// before the controller connected (or while it was disconnected), so a pull
// loop that catches up once and then never ingests anything would still pass
// them (metrics_history#1061).
TEST(IndexerPullController, IngestsLiveCommitsAfterCatchingUpAtGenesis)
{
  TmpDir event_dir;
  TmpDir indexer_dir;
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  EventRPCContext event(event_dir.directory());
  ASSERT_TRUE(event.reconcile_outbox_tip(0, genesis)().has_value());
  EventIndexerServer event_server(event);
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});

  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  IndexerPullControllerOptions options;
  options.retry_delay = std::chrono::milliseconds(1);
  options.caught_up_poll_delay = std::chrono::milliseconds(1);
  IndexerPullController controller(
      std::string(x23::HostPort{"127.0.0.1", event_port}.show()), *db, failures,
      options);
  controller.wait_until_caught_up();
  EXPECT_EQ(*db->latest_block(), 0);

  // The first commit is the regression case: the caught-up poll loop must
  // pick it up on its own, not just find it on a later startup replay.
  const ProposedTransaction first_body = make_body(101);
  ConsensusValue tip = commit_block(event, 1, 1, sha256(genesis), first_body);
  ASSERT_TRUE(
      wait_for_condition([&] { return *db->latest_block() == BlockHeight{1}; },
                         std::chrono::seconds(5)))
      << "indexer never ingested the first live commit";

  // Keep committing without waiting per block, as the running network does.
  std::vector<ProposedTransaction> bodies{first_body};
  constexpr BlockHeight kLastHeight = 4;
  for (BlockHeight height = 2; height <= kLastHeight; ++height) {
    bodies.push_back(make_body(100 + static_cast<std::uint64_t>(height)));
    tip = commit_block(event, height, height, sha256(tip), bodies.back());
  }
  ASSERT_TRUE(
      wait_for_condition([&] { return *db->latest_block() == kLastHeight; },
                         std::chrono::seconds(5)))
      << "indexer stalled at block " << *db->latest_block();
  EXPECT_TRUE(controller.wait_until_caught_up_for(std::chrono::seconds(5)));

  const IndexerProgress progress = db->durable_progress();
  ASSERT_EQ(progress.state(), IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(progress.initialized().contiguous_height, kLastHeight);
  EXPECT_EQ(progress.initialized().last_consensus_hash, sha256(tip));
  for (BlockHeight height = 1; height <= kLastHeight; ++height) {
    const ProposedTransaction &body =
        bodies[static_cast<std::size_t>(height - 1)];
    auto record = db->get_tx_record(sha256(body.signed_tx.tx));
    ASSERT_TRUE(record.has_value()) << record.error();
    EXPECT_EQ(record->height, height);
    expect_xdr_equal(record->tx, body);
  }
}

TEST(IndexerPullController, ReconnectsAfterEventServerTransportLoss)
{
  TmpDir event_dir;
  TmpDir indexer_dir;
  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  EventRPCContext event(event_dir.directory());
  ASSERT_TRUE(event.reconcile_outbox_tip(0, genesis)().has_value());
  ConsensusValue block_one = commit_block(event, 1, 1, sha256(genesis));

  auto event_server = std::make_unique<EventIndexerServer>(event);
  auto [event_port, event_listener] =
      event_server->bind_server(x23::HostPort{"127.0.0.1", 0});
  x23::UniqueFd replacement_listener(
      ::fcntl(event_listener.get(), F_DUPFD_CLOEXEC, 0));
  ASSERT_TRUE(replacement_listener);
  event_server->start_accepting(std::move(event_listener));
  const std::string event_addr =
      std::string(x23::HostPort{"127.0.0.1", event_port}.show());
  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  IndexerPullControllerOptions options;
  options.retry_delay = std::chrono::milliseconds(1);
  options.caught_up_poll_delay = std::chrono::milliseconds(1);
  options.rpc_timeout = std::chrono::seconds(1);
  IndexerPullController controller(event_addr, *db, failures, options);
  controller.wait_until_caught_up();

  event_server.reset();
  ASSERT_TRUE(wait_for_condition(
      [&] {
        return !controller.wait_until_caught_up_for(
            std::chrono::milliseconds(0));
      },
      std::chrono::seconds(5)));
  ConsensusValue block_two = commit_block(event, 2, 2, sha256(block_one));
  event_server = std::make_unique<EventIndexerServer>(event);
  event_server->start_accepting(std::move(replacement_listener));

  ASSERT_TRUE(
      wait_for_condition([&] { return *db->latest_block() == BlockHeight{2}; },
                         std::chrono::seconds(5)));
  const IndexerProgress progress = db->durable_progress();
  ASSERT_EQ(progress.state(), IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(progress.initialized().last_consensus_hash, sha256(block_two));
  EXPECT_TRUE(controller.wait_until_caught_up_for(std::chrono::seconds(5)));
}

TEST(IndexerPullController, RpcTimeoutReconnectsAfterBlockedPull)
{
  TmpDir indexer_dir;
  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  RpcServer<BlockingPullRpc> event_server(BlockingPullRpc::Mode::kBlockFirst);
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  x23::Defer release_server_calls(
      [&] { event_server.impl()->release(&event_server.cp_.pool_); });
  IndexerPullControllerOptions options;
  options.retry_delay = std::chrono::milliseconds(1);
  options.caught_up_poll_delay = std::chrono::seconds(1);
  options.rpc_timeout = std::chrono::milliseconds(20);
  IndexerPullController controller(
      std::string(x23::HostPort{"127.0.0.1", event_port}.show()), *db, failures,
      options);

  const bool reconnected =
      event_server.impl()->wait_for_calls(2, std::chrono::seconds(5));
  event_server.impl()->release(&event_server.cp_.pool_);
  ASSERT_TRUE(reconnected);
  EXPECT_TRUE(controller.wait_until_caught_up_for(std::chrono::seconds(5)));
}

TEST(IndexerPullController, DestructionCancelsBlockedPull)
{
  TmpDir indexer_dir;
  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  RpcServer<BlockingPullRpc> event_server(BlockingPullRpc::Mode::kAlwaysBlock);
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  x23::Defer release_server_calls(
      [&] { event_server.impl()->release(&event_server.cp_.pool_); });
  IndexerPullControllerOptions options;
  options.rpc_timeout = std::chrono::minutes(1);
  auto controller = std::make_unique<IndexerPullController>(
      std::string(x23::HostPort{"127.0.0.1", event_port}.show()), *db, failures,
      options);
  ASSERT_TRUE(event_server.impl()->wait_for_calls(1, std::chrono::seconds(5)));
  EXPECT_FALSE(
      controller->wait_until_caught_up_for(std::chrono::milliseconds(20)));

  auto destroy = std::async(std::launch::async,
                            [controller = std::move(controller)]() mutable {
                              controller.reset();
                            });
  const bool cancelled =
      destroy.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  event_server.impl()->release(&event_server.cp_.pool_);
  destroy.get();
  EXPECT_TRUE(cancelled);
}

} // namespace
} // namespace hogd
