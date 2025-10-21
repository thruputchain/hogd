#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "event.hh"
#include "hogd/base/test.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_sched.h"
#include "rpc_types.hh"

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

  auto result = sched_rpc.eventsched_close_block(1);
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

} // namespace hogd
