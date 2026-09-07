// get_db_tip serving tests. The tip is keyed on the height the caller opened
// its block with, never on per-block snapshot state, so the read cannot race
// the block's own creation -- the scheduler opens executor blocks
// concurrently with dbsched_create_snapshot -- and a commit mid-publication
// suspends the reader instead of parking its worker.
//
// Block ids here are deliberately unrelated to heights: nothing in the
// serving path may assume they coincide.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "consensus.hh"
#include "rpc_types.hh"
#include "types.hh"

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

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

class DbTipServingTest : public ::testing::Test {
protected:
  DbTipServingTest() : ctx_(make_dao(), nullptr), exec_(ctx_) {}

  // CHECKs rather than ASSERTs so it holds on non-test threads too.
  void commit_tip(BlockTXID id, BlockHeight height)
  {
    ConsensusValue value{};
    value.proposal_header.block_height = height;
    CHECK(ctx_.update_dbtip(id, value, StorageLongHash{}).has_value());
  }

  RPCContext ctx_;
  HogDbExecutorRpc exec_;
};

// The core of height-keyed serving: no snapshot has been created for the
// requesting block -- as when the executor's block open outruns
// dbsched_create_snapshot -- and the tip still serves.
TEST_F(DbTipServingTest, ServedWithoutAnyBlockSnapshot)
{
  commit_tip(BlockTXID{700}, 5);

  auto res = exec_.get_db_tip(BlockID{12345}, 6)();
  ASSERT_EQ(res.error(), RPCError::kRPCNone);
  EXPECT_EQ(res.db_tip().id, 700U);
  EXPECT_EQ(res.db_tip().value.proposal_header.block_height, 5);
}

// A block whose slot the chain has already filled (a sibling committed at
// its height), or that sits even further back, is dead: refused, never
// waited for.
TEST_F(DbTipServingTest, RefusedOnceChainMovesPast)
{
  commit_tip(BlockTXID{700}, 5);

  EXPECT_EQ(exec_.get_db_tip(BlockID{12345}, 5)().error(),
            RPCError::kRPCBadBlockID);
  EXPECT_EQ(exec_.get_db_tip(BlockID{12345}, 3)().error(),
            RPCError::kRPCBadBlockID);
}

// A height whose parent never committed is refused promptly rather than
// waited on: with no commit in flight there is nothing to wait for, and a
// block created-then-destroyed before the request arrived must not hang the
// reader.
TEST_F(DbTipServingTest, FutureHeightRefusedNotWaited)
{
  commit_tip(BlockTXID{700}, 5);

  EXPECT_EQ(exec_.get_db_tip(BlockID{12345}, 9)().error(),
            RPCError::kRPCBadBlockID);
}

// Loading a tip for the committed block itself is a caller bug (a view
// opened on the tip's own id); refused server-side to match the
// DatabaseProxy's client-side check.
TEST_F(DbTipServingTest, SelfTipRefused)
{
  commit_tip(BlockTXID{700}, 5);

  EXPECT_EQ(exec_.get_db_tip(BlockID{700}, 6)().error(),
            RPCError::kRPCBadBlockID);
}

// The wait machinery: a reader asking for the tip an in-flight commit is
// about to install suspends through the publication window and serves the
// post-commit tip -- neither the pre-commit tip nor a refusal.
TEST_F(DbTipServingTest, WaitsOutCommitPublication)
{
  commit_tip(BlockTXID{700}, 5);

  auto publication_guard =
      std::make_unique<RPCContext::CommitPublicationGuard>(ctx_,
                                                           BlockTXID{701});
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Production's ordering: the tip advances inside the publication window,
    // then the window closes.
    commit_tip(BlockTXID{701}, 6);
    publication_guard.reset(); // finish_commit_publication
  });

  auto res = exec_.get_db_tip(BlockID{999}, 7)();
  releaser.join();

  ASSERT_EQ(res.error(), RPCError::kRPCNone);
  EXPECT_EQ(res.db_tip().id, 701U);
  EXPECT_EQ(res.db_tip().value.proposal_header.block_height, 6);
}

// The other side of the wake: the commit that finishes publication fills the
// waiter's own slot, so the woken reader must refuse rather than serve.
TEST_F(DbTipServingTest, WokenReaderRefusesWhenItsSlotWasTaken)
{
  commit_tip(BlockTXID{700}, 5);

  auto publication_guard =
      std::make_unique<RPCContext::CommitPublicationGuard>(ctx_,
                                                           BlockTXID{701});
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    commit_tip(BlockTXID{701}, 6);
    publication_guard.reset();
  });

  // Asks for height 6 -- the very height the in-flight commit takes.
  auto res = exec_.get_db_tip(BlockID{999}, 6)();
  releaser.join();

  EXPECT_EQ(res.error(), RPCError::kRPCBadBlockID);
}

} // namespace
} // namespace hogd
