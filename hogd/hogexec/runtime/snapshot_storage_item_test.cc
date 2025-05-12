#include <gtest/gtest.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hogexec/runtime/test_util/queue_util.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

class SnapshotStorageItemTest : public ::testing::Test {
protected:
  static constexpr uint32_t kPortNumber = 12345;
  void SetUp() override
  {
    server_.start_server(kPortNumber);
    db_proxy_ = std::make_unique<DatabaseProxy>(cp_, LocalMockDB{kPortNumber});
  }

  BlockSnapshotServer server_ = BlockSnapshotServer(1);

  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::unique_ptr<DatabaseProxy> db_proxy_;

  std::unique_ptr<SnapshotStorageItem> make_empty_item()
  {
    assert(db_proxy_);
    return std::make_unique<SnapshotStorageItem>(
        StorageItem(StorageItemType::kItemTypeDeleted), AddressAndKey{},
        BlockID{1}, *db_proxy_);
  }
};

TEST_F(SnapshotStorageItemTest, RCQGetBlocks)
{
  // setup an rcq item

  auto queue = make_queue({{make_test_queue_entry(1)},
                           {make_test_queue_entry(2), make_test_queue_entry(3)},
                           {},
                           {make_test_queue_entry(4), make_test_queue_entry(5)},
                           {},
                           {},
                           {make_test_queue_entry(6)},
                           {},
                           {}});

  server_.setup_create_item(AddressAndKey{},
                            StorageItemWrapper(StorageItemWrapper::RetrocausalQueueMarker{}, std::move(queue)));

  SnapshotStorageItem test_item(StorageItem(
                                    StorageItemType::kItemTypeRetrocausalQueue),
                                AddressAndKey{}, BlockID{1}, *db_proxy_);

  auto check_get_block = [&](uint64_t search_block_number,
                             uint64_t expected_block_number,
                             x23::Vec<QueueEntry> expected_entries) {
    auto block = test_item.get_next_rcq_block(search_block_number);
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block.value()->block_number, expected_block_number);
    ASSERT_EQ(block.value()->entries.size(), expected_entries.size());
    for (size_t i = 0; i < expected_entries.size(); ++i) {
      EXPECT_EQ(block.value()->entries[i], expected_entries[i]);
    }
  };

  auto check_get_block_not_found = [&](uint64_t search_block_number) {
    auto block = test_item.get_next_rcq_block(search_block_number);
    ASSERT_TRUE(block.has_value());
    EXPECT_FALSE(block.value());
  };

  check_get_block(1, 1, {make_test_queue_entry(1)});

  check_get_block(4, 4, {make_test_queue_entry(4), make_test_queue_entry(5)});
  check_get_block(3, 4, {make_test_queue_entry(4), make_test_queue_entry(5)});

  check_get_block_not_found(200);
  check_get_block_not_found(8);
  // not a clean way to test this yet, but manual inspection shows that
  // the storage item doesn't make an rpc for this query.
  check_get_block_not_found(9);

  check_get_block(5, 7, {make_test_queue_entry(6)});
}

TEST_F(SnapshotStorageItemTest, RCQGetConcurrentEntries)
{

  SnapshotStorageItem test_item(StorageItem(
                                    StorageItemType::kItemTypeRetrocausalQueue),
                                AddressAndKey{}, BlockID{1}, *db_proxy_);
  auto check_concurrent_entry =
      [&](uint32_t index, std::optional<QueueEntry> const &expected_entry) {
        auto entry = test_item.get_concurrent_rcq_entry(index);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry.value().get(), expected_entry);
      };

  check_concurrent_entry(0, std::nullopt);
  check_concurrent_entry(1, std::nullopt);

  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(1));
  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(2));
  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(3));

  check_concurrent_entry(0, make_test_queue_entry(1));
  check_concurrent_entry(1, make_test_queue_entry(2));
  check_concurrent_entry(2, make_test_queue_entry(3));
  check_concurrent_entry(3, std::nullopt);

  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(4));
  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(5));

  check_concurrent_entry(0, make_test_queue_entry(1));
  check_concurrent_entry(1, make_test_queue_entry(2));
  check_concurrent_entry(2, make_test_queue_entry(3));
  check_concurrent_entry(3, make_test_queue_entry(4));
  check_concurrent_entry(4, make_test_queue_entry(5));
  check_concurrent_entry(5, std::nullopt);
}
} // namespace hogd
