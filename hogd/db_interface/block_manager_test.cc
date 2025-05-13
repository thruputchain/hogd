#include <gtest/gtest.h>

#include <memory>

#include "hogd/db_interface/block_manager.h"
#include "hogd/mockdb/in_memory_database.h"
#include "rpc_types.hh"

namespace hogd {

TEST(BlockManagerTest, CommitCleanup)
{
  constexpr static BlockExecutionMode kMode =
      BlockExecutionMode::kBlockExecutionModeProposal;
  InMemoryDatabase db;
  BlockManager m(0, &db);
  // block 0 doesn't have state that can be manipulated;
  // it's the root of the block tree, and is already committed.
  EXPECT_FALSE(m.get_block_data(0));
  EXPECT_FALSE(m.get_block_data(1));

  EXPECT_TRUE(m.add_child(0, 1, kMode));
  EXPECT_TRUE(m.get_block_data(1));

  // block 10 doesn't exist
  EXPECT_FALSE(m.add_child(10, 11, kMode));
  EXPECT_FALSE(m.get_block_data(11));

  // 2 and 3 should get cleaned up by commit 1
  // 2 gets cleaned up, even though it's been closed (but not committed).
  EXPECT_TRUE(m.add_child(0, 2, kMode));
  auto b2 = m.get_block_data(2);
  b2->close_block();
  EXPECT_TRUE(m.add_child(2, 3, kMode));

  // children of 2 get cleaned up by commit 1
  EXPECT_TRUE(m.add_child(2, 4, kMode));
  EXPECT_TRUE(m.add_child(2, 5, kMode));
  // can't add children to 3, because it hasn't closed yet
  EXPECT_FALSE(m.add_child(3, 6, kMode));

  EXPECT_TRUE(m.get_block_data(2));
  EXPECT_TRUE(m.get_block_data(3));

  m.get_block_data(1)->close_block();
  m.commit_block(1);

  EXPECT_FALSE(m.get_block_data(2));
  EXPECT_FALSE(m.get_block_data(3));
}

TEST(BlockManagerTest, AddSequenceOfChildren)
{
  constexpr static BlockExecutionMode kMode =
      BlockExecutionMode::kBlockExecutionModeProposal;

  InMemoryDatabase db;
  BlockManager m(0, &db);
  EXPECT_TRUE(m.add_child(0, 1, kMode));
  auto b1 = m.get_block_data(1);
  ASSERT_TRUE(b1);

  EXPECT_FALSE(m.commit_block(1));
  b1->close_block();
  EXPECT_TRUE(m.commit_block(1));

  EXPECT_TRUE(m.add_child(1, 2, kMode));
  auto b2 = m.get_block_data(2);
  ASSERT_TRUE(b2);
  b2->close_block();
  EXPECT_TRUE(m.commit_block(2));

  EXPECT_TRUE(m.add_child(2, 3, kMode));
  auto b3 = m.get_block_data(3);
  ASSERT_TRUE(b3);
  b3->close_block();
  EXPECT_TRUE(m.commit_block(3));

  EXPECT_FALSE(m.get_block_data(0));
  EXPECT_FALSE(m.get_block_data(1));
  EXPECT_FALSE(m.get_block_data(2));
  EXPECT_TRUE(m.get_block_data(3));
}

} // namespace hogd
