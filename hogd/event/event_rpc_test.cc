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
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_sched.h"
#include "hogd/event/test_util/socketpair_indexer_client.h"
#include "indexer.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
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
// test); process_events is unused here.
struct FailureCapturingIndexerRpc {
  using rpc_service = IndexerEvent1;

  x23::Async<void>
  indexer_event_process_events(BlockHeight /*block_height*/,
                               const TxEventLogBatch & /*tx_events*/,
                               const ProposedTransactionBatch & /*tx_bodies*/)
  {
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
};
static_assert(is_IndexerEvent1<FailureCapturingIndexerRpc *, x23::Async>);

} // namespace

TEST_F(EventRpcTest, TxFailureReportRelaysToAllIndexers)
{
  EventRPCContext ctx(temp_dir());

  FailureCapturingIndexerRpc indexer_a;
  FailureCapturingIndexerRpc indexer_b;
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_a))();
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_b))();

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
  ctx.add_indexer(dead_stream->client<IndexerEvent1>())();

  // Second indexer: live and capturing.
  FailureCapturingIndexerRpc live_indexer;
  ctx.add_indexer(make_socketpair_indexer_client(&live_indexer))();

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
