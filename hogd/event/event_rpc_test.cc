#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono> // IWYU pragma: keep
#include <filesystem>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_indexer.h"
#include "hogd/event/rpc_from_sched.h"
#include "hogd/event/status.h"
#include "hogd/event/test_util/socketpair_indexer_client.h"
#include "indexer.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/srpc.h"

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

// Minimal IndexerEvent1 fake capturing failure reports (the relay under
// test) and committed-block event batches (the commit fan-out).
struct FailureCapturingIndexerRpc {
  using rpc_service = IndexerEvent1;

  x23::Async<void>
  indexer_event_process_events(BlockHeight block_height,
                               const TxEventLogBatch &tx_events,
                               const ProposedTransactionBatch & /*tx_bodies*/)
  {
    event_batches.emplace_back(block_height, tx_events.size());
    co_return;
  }

  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    reports.emplace_back(tx_hash, diag);
    co_return;
  }

  std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>> reports;
  std::vector<std::pair<BlockHeight, size_t>> event_batches;
};
static_assert(is_IndexerEvent1<FailureCapturingIndexerRpc *, x23::Async>);

} // namespace

// Regression for #2923: an indexer peer that sends an IndexerCard whose
// protocol string does not match the local hogd protocol must be rejected
// gracefully. The protocol string is remote RPC input, not an internal
// invariant, so a mismatch must not abort the event server. The mismatch is
// also detected before any network dial, so the (deliberately unreachable)
// address is never touched.
TEST_F(EventRpcTest, IndexerIntroProtocolMismatchIsRejectedNotAborted)
{
  EventRPCContext ctx(temp_dir());
  EventIndexerRpc indexer_rpc(ctx);
  const IndexerCard card{.protocol = "definitely-not-the-hogd-protocol",
                         .address_for_event = "127.0.0.1:1"};

  const EventIndexerIntroOutcome outcome =
      indexer_rpc.event_indexer_intro(card)();
  EXPECT_EQ(outcome, EventIndexerIntroOutcome::EventIntroMismatchedProtocol);
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
  ctx.add_indexer(dead_stream->client<IndexerEvent1>());

  // Second indexer: live and capturing.
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

TEST_F(EventRpcTest, DeadIndexerDoesNotFailCommit)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  // First "indexer": a client whose peer is closed immediately, so the
  // commit-time process_events send to it fails at the RPC layer.
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  CHECK_EQ(close(fds[1]), 0);
  auto dead_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  ctx.add_indexer(dead_stream->client<IndexerEvent1>());

  // Second indexer: live and capturing.
  FailureCapturingIndexerRpc live_indexer;
  ctx.add_indexer(make_socketpair_indexer_client(&live_indexer));

  EXPECT_EQ(mempool_rpc.eventsched_open_block(7)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(7)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(7, 1, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(7)(), RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_close_block(7, 42)().error(),
            RPCError::kRPCNone);

  // Commit must succeed despite the dead indexer (its send error is dropped),
  // and the live indexer must still receive the block's events.
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(7)(), RPCError::kRPCNone);
  ASSERT_EQ(live_indexer.event_batches.size(), 1);
  EXPECT_EQ(live_indexer.event_batches[0].first, 42);
  EXPECT_EQ(live_indexer.event_batches[0].second, 1);

  // Commit releases the block id, but removal of the context from the map
  // ("last reference out does the GC") may be deferred to close_block's
  // detached file-write coroutine, so an immediate reopen can transiently
  // see kExists. Retry with a deadline; any other error is a real failure.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (true) {
    auto reopen = ctx.open_block(7)();
    if (reopen.has_value()) {
      break;
    }
    ASSERT_EQ(reopen.error().code(), EventErrorCode::kExists);
    ASSERT_LT(std::chrono::steady_clock::now(), deadline)
        << "block id 7 still not reusable long after commit";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(7)(), RPCError::kRPCNone);
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

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{},
                                              ProposedTransaction{})(),
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
  EXPECT_EQ(mempool_rpc.eventsched_close_block(1, 1)().error(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_commit_block(1)(), RPCError::kRPCNone);
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

  EXPECT_EQ(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{},
                                                 ProposedTransaction{}),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_client.eventexecutor_close_block(0), RPCError::kRPCNone);
  EXPECT_EQ(mempool_client.eventsched_close_block(0, 0).error(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_client.eventsched_commit_block(0), RPCError::kRPCNone);
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
  EXPECT_EQ(mempool_client.eventsched_close_block(0, 0).error(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_client.eventsched_commit_block(0), RPCError::kRPCNone);
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
  EXPECT_EQ(mempool_client.eventsched_close_block(0, 0).error(),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_client.eventsched_commit_block(0), RPCError::kRPCNone);
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
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);

  // Close two executors
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  // close_block should wait for the third executor
  std::atomic<bool> close_started = false;
  std::atomic<bool> close_completed = false;
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    close_started.store(true, std::memory_order_release);
    auto result = co_await mempool_rpc.eventsched_close_block(1, 1);
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    close_completed.store(true, std::memory_order_release);
    co_return;
  }();

  // Give close_block time to start
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)
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
  std::atomic<bool> close_completed = false;
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    auto result = co_await mempool_rpc.eventsched_close_block(1, 1);
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    close_completed.store(true, std::memory_order_release);
    co_return;
  }();

  // Give close_block time to start waiting
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)

  // close_block should not have completed yet
  EXPECT_FALSE(close_completed.load(std::memory_order_acquire));

  // Add an event and close executor
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{},
                                              ProposedTransaction{})(),
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
TEST_F(EventRpcTest, AbandonWhileCloseBlockWaiting)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);

  // Start close_block in background (will wait for executor)
  std::atomic<bool> close_completed = false;
  auto async = [&](this auto) -> x23::Async<void> {
    co_await default_threadpool().bg();
    auto result = co_await mempool_rpc.eventsched_close_block(1, 1);
    // Should get an error because block was abandoned
    EXPECT_NE(result.error(), RPCError::kRPCNone);
    close_completed.store(true);
  }();

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)

  // Abandon the block while close_block is waiting
  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);

  EXPECT_NE(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  // close_block should complete (with error) instead of hanging
  async();
  EXPECT_TRUE(close_completed.load());
}

// Regression for #3039: closing a block that was abandoned while a context
// reference was still outstanding must fail with kBadBlockID. The abandon
// leaves the context in the map in ARC-shutdown mode (initiate_shutdown()
// cannot destroy it while the reference is live), so close's
// wait_for_executors_closed() fails to acquire a reference -- it must report
// that as kBadBlockID, not release a reference it never acquired (which
// would drop the shutdown refcount to zero and free the context out from
// under the live holder).
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

// Companion to CloseAfterAbandonWithOutstandingReferenceIsRejected (#3039),
// one stage later: an abandon can land in the gap between close_block's
// wait_for_executors_closed() succeeding and its re-acquisition of the block
// map lock, leaving an abandoned, shutdown-pending context (pinned in the
// map by an outstanding reference) that close_block then finds. close_block
// must detect this and return kBadBlockID -- not fail-stop on an
// unacquirable reference or run the finish pipeline on an abandoned block.
// The gap is a few instructions wide, so this hammers the interleaving with
// a swept timing offset; every outcome must be a clean error code.
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
    auto close_task = [&](this auto) -> x23::Async<void> {
      co_await default_threadpool().bg();
      auto result = co_await mempool_rpc.eventsched_close_block(block_id, 1);
      close_err.store(result.error(), std::memory_order_release);
      co_return;
    }();

    // Let close_block reach its executor wait, then release the last
    // executor handle (waking it) and immediately race an abandon against
    // its map-lock re-acquisition, sweeping the offset across iterations.
    std::this_thread::sleep_for(2ms); // NOLINT(misc-include-cleaner)
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

  for (BlockID block_id = 1; block_id <= 3; block_id++) {
    EXPECT_EQ(mempool_rpc.eventsched_open_block(block_id)(),
              RPCError::kRPCNone);
    EXPECT_EQ(exec_rpc.eventexecutor_open_block(block_id)(),
              RPCError::kRPCNone);

    EXPECT_EQ(exec_rpc.eventexecutor_emit_event(block_id, 1, TxEventLog{},
                                                ProposedTransaction{})(),
              RPCError::kRPCNone);

    EXPECT_EQ(exec_rpc.eventexecutor_close_block(block_id)(),
              RPCError::kRPCNone);

    auto result = mempool_rpc.eventsched_close_block(block_id, block_id)();
    EXPECT_EQ(result.error(), RPCError::kRPCNone);
    EXPECT_EQ(mempool_rpc.eventsched_commit_block(block_id)(),
              RPCError::kRPCNone);
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

TEST_F(EventRpcTest, AbandonClosedBlockDeletesFinalFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1, 1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  auto final_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());
  auto temp_file = EventBlockContext::temp_events_filename(temp_dir(), 1);

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1)(), RPCError::kRPCNone);
  EXPECT_FALSE(std::filesystem::exists(final_file));
  EXPECT_FALSE(std::filesystem::exists(temp_file));
}

TEST_F(EventRpcTest, CommitBlockKeepsFinalFile)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_open_block(1)(), RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{},
                                              ProposedTransaction{})(),
            RPCError::kRPCNone);
  EXPECT_EQ(exec_rpc.eventexecutor_close_block(1)(), RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1, 1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  auto final_file =
      EventBlockContext::final_events_filename(temp_dir(), result.root_hash());

  EXPECT_EQ(mempool_rpc.eventsched_commit_block(1)(), RPCError::kRPCNone);
  EXPECT_TRUE(std::filesystem::exists(final_file));
}

} // namespace hogd
