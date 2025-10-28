#include <gtest/gtest.h>

#include <filesystem>

#include "hogd/indexer/db.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"

namespace hogd {

// basic tests on open/close db ops
class IndexDBTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // Create a unique temporary directory for each test
    test_dir_ = std::filesystem::temp_directory_path() / "indexdb_test";
    std::filesystem::remove_all(test_dir_);
  }

  void TearDown() override
  {
    // Clean up test directory
    std::filesystem::remove_all(test_dir_);
  }

  std::filesystem::path test_dir_;
};

// Test creating a new database
TEST_F(IndexDBTest, CreateNew)
{
  auto db = IndexDB::make_new(test_dir_);
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db->is_open());
}

// Test that creating a new database fails if one already exists
TEST_F(IndexDBTest, CreateNewFailsIfExists)
{
  {
    auto db1 = IndexDB::make_new(test_dir_);
    ASSERT_TRUE(db1.has_value());
  }

  // Try to create another database at the same path (should fail)
  auto db2 = IndexDB::make_new(test_dir_);
  EXPECT_FALSE(db2.has_value());
  EXPECT_EQ(db2.error(), IndexerErrorCode::kDBAlreadyExists);
}

// Test opening an existing database
TEST_F(IndexDBTest, OpenExisting)
{
  // Create a database first
  {
    auto db = IndexDB::make_new(test_dir_);
    ASSERT_TRUE(db.has_value());
  }

  // Now open it
  auto db = IndexDB::open(test_dir_);
  ASSERT_TRUE(db.has_value());
  EXPECT_TRUE(db->is_open());
}

// Test that opening a non-existent database fails
TEST_F(IndexDBTest, OpenNonExistent)
{
  auto db = IndexDB::open(test_dir_);
  EXPECT_FALSE(db.has_value());
  EXPECT_EQ(db.error(), IndexerErrorCode::kDBNotFound);
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
