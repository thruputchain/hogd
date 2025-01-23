#include <gtest/gtest.h>

#include "hogd/test/local_test_harness.h"

namespace hogd {

class TestCoordToDb : public ::testing::Test, public test::LocalTestHarness {

protected:
  void SetUp() override {}
};

TEST_F(TestCoordToDb, MultipleSnapshots)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  db_client_.close_snapshot(1)();
  db_client_.commit_snapshot(1)();
  ASSERT_EQ(db_client_.open_database_snapshot(1, 2)(), RPCError::kNone);
  db_client_.close_snapshot(2)();
  db_client_.commit_snapshot(2)();
  ASSERT_EQ(db_client_.open_database_snapshot(2, 3)(), RPCError::kNone);
  db_client_.close_snapshot(3)();
  db_client_.commit_snapshot(3)();
  ASSERT_EQ(db_client_.open_database_snapshot(3, 4)(), RPCError::kNone);
  db_client_.close_snapshot(4)();
  db_client_.commit_snapshot(4)();
  ASSERT_EQ(db_client_.open_database_snapshot(4, 5)(), RPCError::kNone);
  db_client_.close_snapshot(5)();
  db_client_.commit_snapshot(5)();
}

TEST_F(TestCoordToDb, BranchingSnapshots)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(0, 2)(), RPCError::kNone);
  ASSERT_EQ(db_client_.open_database_snapshot(0, 3)(), RPCError::kNone);
  db_client_.close_snapshot(1)();
  db_client_.close_snapshot(2)();
  db_client_.commit_snapshot(2)();
  ASSERT_NE(db_client_.open_database_snapshot(1, 4)(), RPCError::kNone);
  ASSERT_NE(db_client_.open_database_snapshot(3, 5)(), RPCError::kNone);

  ASSERT_EQ(db_client_.open_database_snapshot(2, 6)(), RPCError::kNone);
  db_client_.close_snapshot(6)();
  db_client_.commit_snapshot(6)();
}

} // namespace hogd
