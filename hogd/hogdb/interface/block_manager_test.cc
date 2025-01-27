#include <gtest/gtest.h>

#include "hogd/hogdb/interface/block_manager.h"

namespace hogd {

TEST(BlockManagerTest, CommitCleanup)
{
  InMemoryDatabase db;
  BlockManager m(0, std::make_shared<BlockState>(&db), &db);

  EXPECT_TRUE(m.get_block_data(0));
  EXPECT_FALSE(m.get_block_data(1));

  auto b0 = m.get_block_data(0);
  b0->close_block();

  EXPECT_TRUE(m.add_child(0, 1, std::make_shared<BlockState>(b0)));
  EXPECT_TRUE(m.get_block_data(1));

  // block 10 doesn't exist
  EXPECT_FALSE(m.add_child(10, 11, std::make_shared<BlockState>(nullptr)));
  EXPECT_FALSE(m.get_block_data(11));

  // 2 and 3 should get cleaned up by commit 1
  EXPECT_TRUE(m.add_child(0, 2, std::make_shared<BlockState>(b0)));
  auto b2 = m.get_block_data(2);
  b2->close_block();
  EXPECT_TRUE(m.add_child(2, 3, std::make_shared<BlockState>(b2)));

  EXPECT_TRUE(m.get_block_data(2));
  EXPECT_TRUE(m.get_block_data(3));

  m.get_block_data(1)->close_block();
  m.commit_block(1);

  EXPECT_FALSE(m.get_block_data(2));
  EXPECT_FALSE(m.get_block_data(3));
}

TEST(BlockManagerTest, AddSequenceOfChildren)
{
  InMemoryDatabase db;
  BlockManager m(0, std::make_shared<BlockState>(&db), &db);
  auto b0 = m.get_block_data(0);
  b0->close_block();
  EXPECT_TRUE(m.add_child(0, 1, std::make_shared<BlockState>(b0)));
  auto b1 = m.get_block_data(1);
  ASSERT_TRUE(b1);
  b1->close_block();
  m.commit_block(1);

  EXPECT_TRUE(m.add_child(1, 2, std::make_shared<BlockState>(b1)));
  auto b2 = m.get_block_data(2);
  ASSERT_TRUE(b2);
  b2->close_block();
  m.commit_block(2);

  EXPECT_TRUE(m.add_child(2, 3, std::make_shared<BlockState>(b2)));
  auto b3 = m.get_block_data(3);
  ASSERT_TRUE(b3);
  b3->close_block();
  m.commit_block(3);
}

} // namespace hogd
