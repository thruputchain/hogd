#include "hogd/hog_exec/runtime/item/shard_fanout.h"

#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "rpc_types.hh" // RPCError, BlockID, TransactionID

namespace hogd {
namespace {

// Minimal stand-in for a per-shard db client: records each pop and returns a
// canned RPCError, so a failure can be injected on any chosen shard.
struct MockShardClient {
  RPCError pop_result = RPCError::kRPCNone;
  int pop_calls = 0;

  RPCError pop_subtransactions(BlockID /*block_id*/,
                               TransactionID /*transaction_id*/,
                               uint32_t /*count*/)
  {
    ++pop_calls;
    return pop_result;
  }
};

constexpr BlockID kBlock = 7;
constexpr TransactionID kTx = 3;

TEST(ShardFanout, PopsEveryShardAndReturnsNoneWhenAllOk)
{
  std::vector<MockShardClient> clients(3);
  RPCError err = pop_from_shards(std::span(clients), kBlock, kTx, /*count=*/1);
  EXPECT_EQ(err, RPCError::kRPCNone);
  for (auto const &c : clients) {
    EXPECT_EQ(c.pop_calls, 1);
  }
}

// The bug this guards against: a pop that fails on a middle shard must not stop
// the unwind, or the later shards keep the frame the failed push added and
// their savepoint stacks desync.
TEST(ShardFanout, VisitsAllShardsEvenWhenAMiddlePopFails)
{
  std::vector<MockShardClient> clients(4);
  clients[1].pop_result = RPCError::kRPCTransactionFailed;

  RPCError err = pop_from_shards(std::span(clients), kBlock, kTx, /*count=*/1);

  EXPECT_NE(err, RPCError::kRPCNone); // the failure is surfaced
  for (auto const &c : clients) {
    EXPECT_EQ(c.pop_calls, 1); // and every shard was still popped
  }
}

TEST(ShardFanout, ReturnsFirstErrorNotLast)
{
  std::vector<MockShardClient> clients(3);
  clients[1].pop_result = RPCError::kRPCBadBlockID;
  clients[2].pop_result = RPCError::kRPCTransactionFailed;

  RPCError err = pop_from_shards(std::span(clients), kBlock, kTx, /*count=*/1);

  EXPECT_EQ(err, RPCError::kRPCBadBlockID); // first non-OK, not the last
  for (auto const &c : clients) {
    EXPECT_EQ(c.pop_calls, 1);
  }
}

TEST(ShardFanout, SingleShardPassesThrough)
{
  std::vector<MockShardClient> clients(1);
  EXPECT_EQ(pop_from_shards(std::span(clients), kBlock, kTx, 2),
            RPCError::kRPCNone);
  EXPECT_EQ(clients[0].pop_calls, 1);

  clients[0].pop_result = RPCError::kRPCBadBlockID;
  EXPECT_EQ(pop_from_shards(std::span(clients), kBlock, kTx, 2),
            RPCError::kRPCBadBlockID);
}

} // namespace
} // namespace hogd
