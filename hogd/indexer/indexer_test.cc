#include <algorithm>
#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <vector>

#include "event.hh"
#include "hogd/base/test.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

// basic tests on open/close db ops
class IndexerTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // Ensure the test directory exists for the database under test
    std::filesystem::create_directories(tmp_dir_.directory());
  }

  TmpDir tmp_dir_;
};

// Test creating a new database
TEST_F(IndexerTest, CreateNew)
{
  auto db = IndexDB::make_new(tmp_dir_.directory());
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db->is_open());
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
