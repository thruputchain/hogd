// Lifetime tests for RPCContext's InvokeTX / BlockTX reference management
// (#2959).

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "rpc_types.hh"

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/status.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {

constexpr std::size_t kTestRamBytes = 64UL * 1024 * 1024;

HogDbDAO
make_dao()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(),
                     Database::estimate_heap_size(kTestRamBytes), 0);
  CHECK(db.has_value());
  return HogDbDAO(RAMDB(std::move(*db)));
}

std::unique_ptr<RPCContext>
make_context()
{
  auto ctx = std::make_unique<RPCContext>(make_dao(), nullptr);
  ctx->initialize_cost_model(StorageCostModel::default_cost_model());
  return ctx;
}

} // namespace

// An abandoned block with outstanding invocations must be garbage collected
// exactly when the last invocation is closed.
TEST(RPCContextLifetimeTest, AbandonedBlockFreedAfterLastInvokeTxClose)
{
  std::unique_ptr<RPCContext> ctx = make_context();
  ASSERT_TRUE(
      ctx->open_block_tx(1, BlockExecutionMode::kBlockExecutionModeProposal));

  InvokeTX *first = ctx->get_invoke_tx(1, 1);
  InvokeTX *second = ctx->get_invoke_tx(1, 2);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  PerThreadARC<BlockTX> *block = ctx->get_block_tx(1, false);
  ASSERT_NE(block, nullptr);
  ASSERT_FALSE(block->initiate_shutdown());

  ctx->close_invoke_tx(1, 1, first);
  EXPECT_NE(ctx->get_block_tx(1, false), nullptr);

  ctx->close_invoke_tx(1, 2, second);
  EXPECT_EQ(ctx->get_block_tx(1, false), nullptr);
}

// Regression test for #2959: concurrent closes of distinct invocations on an
// abandoned block must not let one close free the BlockTX while another
// close's InvokeTX destructor can still dereference it. Run under ASan/TSan
// to detect the use-after-free.
TEST(RPCContextLifetimeTest, ConcurrentClosesOfAbandonedBlockAreClean)
{
  std::unique_ptr<RPCContext> ctx = make_context();

  constexpr int kIterations = 128;
  for (int i = 1; i <= kIterations; ++i) {
    BlockTXID bid = i;
    ASSERT_TRUE(
        ctx->open_block_tx(bid,
                           BlockExecutionMode::kBlockExecutionModeProposal));

    InvokeTX *first = ctx->get_invoke_tx(bid, 1);
    InvokeTX *second = ctx->get_invoke_tx(bid, 2);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    PerThreadARC<BlockTX> *block = ctx->get_block_tx(bid, false);
    ASSERT_NE(block, nullptr);
    ASSERT_FALSE(block->initiate_shutdown());

    std::atomic<int> ready{0};
    auto racing_close = [&](InvokeTXID iid, InvokeTX *itx) {
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      ctx->close_invoke_tx(bid, iid, itx);
    };
    std::thread closer_a(racing_close, 1, first);
    std::thread closer_b(racing_close, 2, second);
    closer_a.join();
    closer_b.join();

    EXPECT_EQ(ctx->get_block_tx(bid, false), nullptr);
  }
}

// Destroying the RPCContext while an abandoned block still has an outstanding
// invocation must tear down cleanly: the leftover InvokeTX's reference is
// dropped and the block is reclaimed during destruction.
TEST(RPCContextLifetimeTest, TeardownWithOutstandingInvokeTxOnAbandonedBlock)
{
  std::unique_ptr<RPCContext> ctx = make_context();
  ASSERT_TRUE(
      ctx->open_block_tx(1, BlockExecutionMode::kBlockExecutionModeProposal));
  ASSERT_NE(ctx->get_invoke_tx(1, 1), nullptr);

  PerThreadARC<BlockTX> *block = ctx->get_block_tx(1, false);
  ASSERT_NE(block, nullptr);
  ASSERT_FALSE(block->initiate_shutdown());

  ctx.reset();
}

} // namespace hogd
