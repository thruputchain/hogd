#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_sched.h"
#include "rpc_types.hh"
#include "x23/srpc.h"

namespace hogd {

class EventRpcTest : public ::testing::Test {
public:
  EventRpcTest() = default;
  void SetUp() override { temp_dir_ = std::make_unique<TmpDir>(); }
  void TearDown() override { temp_dir_.reset(); }

  std::string temp_dir() const
  {
    info("temp dir: {}", temp_dir_->directory());
    return temp_dir_->directory();
  }

private:
  std::unique_ptr<TmpDir> temp_dir_;
};

TEST_F(EventRpcTest, BasicUsage)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_open_block(1), RPCError::kRPCNone);

  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(2, 2, TxEventLog{}),
            RPCError::kRPCNone);

  auto result = mempool_rpc.eventsched_close_block(1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_NE(mempool_rpc.eventsched_abandon_block(1), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestAbandonBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1), RPCError::kRPCNone);

  // the tx ctx should keep the block open, at least to keep the pointer alive
  auto *tx_ctx = ctx.get_context(1);
  EXPECT_NE(tx_ctx, nullptr);

  EXPECT_EQ(mempool_rpc.eventsched_abandon_block(1), RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{}),
            RPCError::kRPCNone);

  tx_ctx->get().add_event(TxEventLog{});

  ASSERT_TRUE(tx_ctx->release_reference());
  ctx.gc_context(1);
}

TEST_F(EventRpcTest, TestEmptyBlock)
{
  EventRPCContext ctx(temp_dir());
  EventExecRpc exec_rpc(ctx);
  EventMempoolRpc mempool_rpc(ctx);

  EXPECT_EQ(mempool_rpc.eventsched_open_block(1), RPCError::kRPCNone);
  EXPECT_EQ(mempool_rpc.eventsched_close_block(1)().error(),
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

  EXPECT_NE(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{}),
            RPCError::kRPCNone);

  EXPECT_EQ(mempool_client.eventsched_open_block(0), RPCError::kRPCNone);

  EXPECT_EQ(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_EQ(mempool_client.eventsched_close_block(0).error(),
            RPCError::kRPCNone);
}

} // namespace hogd
