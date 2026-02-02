#include <algorithm>
#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <vector>

#include "event.hh"
#include "hogd/base/test.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/rpc_from_event.h"
#include "hogd/indexer/status.h"
#include "hogd/init.h"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class IndexerTest : public ::testing::Test {
public:
  IndexDB &db() { return *db_; }
  IndexerEventRpc &event() { return *event_; }
  IndexerBlockExplorerRpc &block_explorer() { return *block_explorer_; }

protected:
  void SetUp() override
  {
    // Create a temporary test directory for the database.
    std::filesystem::create_directories(tmp_dir_.directory());
    IndexerStatus<IndexDB> db_status =
        IndexDB::open_or_make_new(tmp_dir_.directory());
    if (!db_status) {
      panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
    }
    IndexDB &db = db_status.value();
    db_ = std::make_unique<IndexDB>(std::move(db));

    event_ = std::make_unique<IndexerEventRpc>(db);
    block_explorer_ = std::make_unique<IndexerBlockExplorerRpc>(db);
  }

  TmpDir tmp_dir_;
  std::unique_ptr<IndexDB> db_;
  std::unique_ptr<IndexerEventRpc> event_;
  std::unique_ptr<IndexerBlockExplorerRpc> block_explorer_;
};

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
