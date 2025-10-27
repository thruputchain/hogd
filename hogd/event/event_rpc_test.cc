#include <filesystem>
#include <gtest/gtest.h>
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
  void SetUp() override { temp_dir_ = TmpDir().directory(); }
  void TearDown() override { std::filesystem::remove_all(temp_dir_); }
  std::string temp_dir_;
};

TEST_F(EventRpcTest, BasicUsage)
{
  EventRPCContext ctx(temp_dir_);
  EventExecRpc exec_rpc(ctx);
  EventSchedRpc sched_rpc(ctx);

  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_EQ(sched_rpc.eventsched_open_block(1), RPCError::kRPCNone);

  EXPECT_EQ(exec_rpc.eventexecutor_emit_event(1, 1, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(2, 2, TxEventLog{}),
            RPCError::kRPCNone);

  auto result = sched_rpc.eventsched_close_block(1)();
  EXPECT_EQ(result.error(), RPCError::kRPCNone);

  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_NE(sched_rpc.eventsched_abandon_block(1), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestAbandonBlock)
{
  EventRPCContext ctx(temp_dir_);
  EventExecRpc exec_rpc(ctx);
  EventSchedRpc sched_rpc(ctx);

  EXPECT_EQ(sched_rpc.eventsched_open_block(1), RPCError::kRPCNone);

  // the tx ctx should keep the block open, at least to keep the pointer alive
  auto *tx_ctx = ctx.get_context(1);
  EXPECT_NE(tx_ctx, nullptr);

  EXPECT_EQ(sched_rpc.eventsched_abandon_block(1), RPCError::kRPCNone);
  EXPECT_NE(exec_rpc.eventexecutor_emit_event(1, 2, TxEventLog{}),
            RPCError::kRPCNone);

  tx_ctx->get().add_event(TxEventLog{});

  ASSERT_TRUE(tx_ctx->release_reference());
  ctx.gc_context(1);
}

TEST_F(EventRpcTest, TestEmptyBlock)
{
  EventRPCContext ctx(temp_dir_);
  EventExecRpc exec_rpc(ctx);
  EventSchedRpc sched_rpc(ctx);

  EXPECT_EQ(sched_rpc.eventsched_open_block(1), RPCError::kRPCNone);
  EXPECT_EQ(sched_rpc.eventsched_close_block(1)().error(), RPCError::kRPCNone);
}

TEST_F(EventRpcTest, TestFromRPC)
{
  EventRPCContext ctx(temp_dir_);
  RpcServer<EventExecRpc> exec_rpc(ctx);
  RpcServer<EventSchedRpc> sched_rpc(ctx);

  auto exec_port = exec_rpc.start_server(0);
  auto sched_port = sched_rpc.start_server(0);

  auto exec_addr = host_port_to_sockaddr("localhost", exec_port);
  auto sched_addr = host_port_to_sockaddr("localhost", sched_port);

  auto exec_client = x23::SRpcMulticlient<x23::RpcLogger>::make(exec_addr)
                         ->client<EventExecutor1>();
  auto sched_client = x23::SRpcMulticlient<x23::RpcLogger>::make(sched_addr)
                          ->client<EventSched1>();

  EXPECT_NE(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{}),
            RPCError::kRPCNone);

  EXPECT_EQ(sched_client.eventsched_open_block(0), RPCError::kRPCNone);

  EXPECT_EQ(exec_client.eventexecutor_emit_event(0, 0, TxEventLog{}),
            RPCError::kRPCNone);
  EXPECT_EQ(sched_client.eventsched_close_block(0).error(), RPCError::kRPCNone);
}

} // namespace hogd
