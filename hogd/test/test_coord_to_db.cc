#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/test/local_test_harness.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class TestCoordToDb : public ::testing::Test, public test::LocalTestHarness {

protected:
  void SetUp() override {}
};

TEST_F(TestCoordToDb, MultipleSnapshots)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  db_client_.commit_snapshot(1)();
  ASSERT_EQ(db_client_.open_database_snapshot(1, 2)(), RPCError::kNone);
  db_client_.commit_snapshot(2)();
  ASSERT_EQ(db_client_.open_database_snapshot(2, 3)(), RPCError::kNone);
  db_client_.commit_snapshot(3)();
  ASSERT_EQ(db_client_.open_database_snapshot(3, 4)(), RPCError::kNone);
  db_client_.commit_snapshot(4)();
  ASSERT_EQ(db_client_.open_database_snapshot(4, 5)(), RPCError::kNone);
  db_client_.commit_snapshot(5)();
}

} // namespace hogd
