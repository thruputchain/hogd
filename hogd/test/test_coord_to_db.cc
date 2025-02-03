#include <gtest/gtest.h>

#include "hogd/test/local_test_harness.h"
#include "rpc_types.hh"

namespace hogd {

class TestCoordToDb : public ::testing::Test, public test::LocalTestHarness {

protected:
  void SetUp() override {}
};

TEST_F(TestCoordToDb, MultipleSnapshots)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(1)(), RPCError::kNone);
  db_client_.commit_snapshot(1)();

  ASSERT_EQ(db_client_.open_database_snapshot(1, 2)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(2)(), RPCError::kNone);
  db_client_.commit_snapshot(2)();

  ASSERT_EQ(db_client_.open_database_snapshot(2, 3)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(3)(), RPCError::kNone);
  db_client_.commit_snapshot(3)();

  ASSERT_EQ(db_client_.open_database_snapshot(3, 4)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(4)(), RPCError::kNone);
  db_client_.commit_snapshot(4)();

  ASSERT_EQ(db_client_.open_database_snapshot(4, 5)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(5)(), RPCError::kNone);
  db_client_.commit_snapshot(5)();
}

TEST_F(TestCoordToDb, BranchingSnapshots)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(0, 2)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(0, 3)(), RPCError::kNone);

  ASSERT_EQ(db_client_.close_snapshot(1)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(2)(), RPCError::kNone);
  db_client_.commit_snapshot(2)();

  ASSERT_NE(db_client_.open_database_snapshot(1, 4)(), RPCError::kNone);
  ASSERT_NE(db_client_.open_database_snapshot(3, 5)(), RPCError::kNone);

  ASSERT_EQ(db_client_.open_database_snapshot(2, 6)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(6)(), RPCError::kNone);
  db_client_.commit_snapshot(6)();
}

TEST_F(TestCoordToDb, DelayedCommit)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(0, 2)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(0, 3)(), RPCError::kNone);

  ASSERT_EQ(db_client_.close_snapshot(1)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(2)(), RPCError::kNone);
  ASSERT_EQ(db_client_.close_snapshot(3)(), RPCError::kNone);

  ASSERT_EQ(db_client_.open_database_snapshot(1, 11)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(2, 21)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(3, 31)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(1, 12)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(2, 22)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(3, 32)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(1, 13)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(2, 23)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(3, 33)(), RPCError::kNone);

  db_client_.close_snapshot(11)();
  db_client_.close_snapshot(21)();
  db_client_.close_snapshot(31)();
  db_client_.close_snapshot(12)();
  db_client_.close_snapshot(22)();
  db_client_.close_snapshot(32)();
  db_client_.close_snapshot(13)();
  db_client_.close_snapshot(23)();
  db_client_.close_snapshot(33)();

  db_client_.commit_snapshot(2)();
  db_client_.commit_snapshot(23)();

  ASSERT_NE(db_client_.open_database_snapshot(1, 14)(), RPCError::kNone);
  ASSERT_NE(db_client_.open_database_snapshot(2, 24)(), RPCError::kNone);
  ASSERT_NE(db_client_.open_database_snapshot(3, 34)(), RPCError::kNone);

  ASSERT_NE(db_client_.open_database_snapshot(31, 311)(), RPCError::kNone);
  ASSERT_NE(db_client_.open_database_snapshot(32, 321)(), RPCError::kNone);
  ASSERT_NE(db_client_.open_database_snapshot(33, 331)(), RPCError::kNone);

  ASSERT_EQ(db_client_.open_database_snapshot(23, 321)(), RPCError::kNone);
}

} // namespace hogd
