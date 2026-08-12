#include <gtest/gtest.h>

#include <cassert>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/item_view_type.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hog_exec/runtime/test_util/queue_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class SnapshotStorageItemTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // We call [server_.start_server()] before constructing [db_proxy_] because
    // we need to pass [server_]'s port to the proxy.
    server_.start_server(/*port=*/0);
    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = server_.port()});
  }

  BlockSnapshotServer server_ = BlockSnapshotServer(/*block_id=*/1);

  std::unique_ptr<DatabaseProxy> db_proxy_;

  std::unique_ptr<SnapshotStorageItem> make_empty_item()
  {
    assert(db_proxy_);
    AddressAndKey addrkey{};
    auto entry = SnapshotStorageItem::make_variant(
        WireStorageItem(StorageItemType::kItemTypeDeleted), addrkey.key);
    CHECK(entry.has_value());
    return std::make_unique<SnapshotStorageItem>(std::move(entry).value(),
                                                 std::move(addrkey), BlockID{1},
                                                 *db_proxy_);
  }
};

TEST_F(SnapshotStorageItemTest, AuthDataMakeVariant)
{
  AddressAndKey addrkey{.address = {},
                        .key = StorageKey(StorageKeyType::AuthData)};

  WireStorageItem wire_item(StorageItemType::kItemTypeAuthData);
  wire_item.auth_data().last_written_height = 42;

  auto entry =
      SnapshotStorageItem::make_variant(std::move(wire_item), addrkey.key);
  ASSERT_TRUE(entry.has_value());
  SnapshotStorageItem item(std::move(entry).value(), std::move(addrkey),
                           BlockID{1}, *db_proxy_);
  EXPECT_EQ(item.type(), ItemViewType::kAddressAuthData);
  EXPECT_EQ(item.auth_data().last_written_height, 42U);
}

TEST_F(SnapshotStorageItemTest, AuthDataDeletedMakeVariant)
{
  AddressAndKey addrkey{.address = {},
                        .key = StorageKey(StorageKeyType::AuthData)};

  WireStorageItem wire_item(StorageItemType::kItemTypeDeleted);

  auto entry =
      SnapshotStorageItem::make_variant(std::move(wire_item), addrkey.key);
  ASSERT_TRUE(entry.has_value());
  SnapshotStorageItem item(std::move(entry).value(), std::move(addrkey),
                           BlockID{1}, *db_proxy_);
  EXPECT_EQ(item.type(), std::nullopt);
}

TEST_F(SnapshotStorageItemTest, RCQGetBlocks)
{
  // setup an rcq item

  auto queue = make_queue({
      {make_test_queue_entry(1)},
      {make_test_queue_entry(2), make_test_queue_entry(3)},
      {make_test_queue_entry(4), make_test_queue_entry(5)},
      {make_test_queue_entry(6)},
  });

  StorageCostModel cost_model(12000, 1, 5, 5, 1234);
  server_->impl()->setup_create_item(AddressAndKey{},
                                     StorageItemWrapper(std::move(queue),
                                                        &cost_model));

  AddressAndKey addrkey{};
  auto entry = SnapshotStorageItem::make_variant(
      WireStorageItem(StorageItemType::kItemTypeRetrocausalQueue), addrkey.key);
  ASSERT_TRUE(entry.has_value());
  SnapshotStorageItem test_item(std::move(entry).value(), std::move(addrkey),
                                BlockID{1}, *db_proxy_);

  auto check_get_block = [&](uint64_t search_block_number,
                             uint64_t expected_block_number,
                             x23::Vec<QueueEntry> expected_entries) {
    auto block = test_item.get_next_rcq_block(search_block_number);
    // CHECK, not ASSERT_*: a fatal gtest assertion inside a lambda returns from
    // the lambda, not the test, so the checks below would still run on a null
    // or short block.
    CHECK(block.has_value());
    CHECK(block.value() != nullptr);
    EXPECT_EQ(block.value()->block_number, expected_block_number);
    CHECK_EQ(block.value()->entries.size(), expected_entries.size());
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

  check_get_block(3, 3, {make_test_queue_entry(4), make_test_queue_entry(5)});

  check_get_block_not_found(200);
  check_get_block_not_found(8);
  // not a clean way to test this yet, but manual inspection shows that
  // the storage item doesn't make an rpc for this query.
  check_get_block_not_found(9);

  check_get_block(4, 4, {make_test_queue_entry(6)});
}

TEST_F(SnapshotStorageItemTest, RCQGetConcurrentEntries)
{

  AddressAndKey addrkey{};
  auto entry = SnapshotStorageItem::make_variant(
      WireStorageItem(StorageItemType::kItemTypeRetrocausalQueue), addrkey.key);
  ASSERT_TRUE(entry.has_value());
  SnapshotStorageItem test_item(std::move(entry).value(), std::move(addrkey),
                                BlockID{1}, *db_proxy_);
  auto check_concurrent_entry =
      [&](uint32_t index, std::optional<QueueEntry> const &expected_entry) {
        auto entry = test_item.get_concurrent_rcq_entry(index);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry.value().get(), expected_entry);
      };

  check_concurrent_entry(0, std::nullopt);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(1));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(2));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(3));

  check_concurrent_entry(0, make_test_queue_entry(1));
  check_concurrent_entry(1, make_test_queue_entry(2));
  check_concurrent_entry(2, make_test_queue_entry(3));
  check_concurrent_entry(3, std::nullopt);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(4));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(5));

  check_concurrent_entry(0, make_test_queue_entry(1));
  check_concurrent_entry(1, make_test_queue_entry(2));
  check_concurrent_entry(2, make_test_queue_entry(3));
  check_concurrent_entry(3, make_test_queue_entry(4));
  check_concurrent_entry(4, make_test_queue_entry(5));
  check_concurrent_entry(5, std::nullopt);
}

TEST_F(SnapshotStorageItemTest, RCQConcurrentEntryErrorGuards)
{

  AddressAndKey addrkey{};
  auto entry = SnapshotStorageItem::make_variant(
      WireStorageItem(StorageItemType::kItemTypeRetrocausalQueue), addrkey.key);
  ASSERT_TRUE(entry.has_value());
  SnapshotStorageItem test_item(std::move(entry).value(), std::move(addrkey),
                                BlockID{1}, *db_proxy_);
  auto check_concurrent_entry =
      [&](uint32_t index, std::optional<QueueEntry> const &expected_entry) {
        auto entry = test_item.get_concurrent_rcq_entry(index);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry.value().get(), expected_entry);
      };

  auto check_concurrent_entry_bad = [&](uint32_t index) {
    auto entry = test_item.get_concurrent_rcq_entry(index);
    ASSERT_FALSE(entry.has_value());
    EXPECT_EQ(entry.error().error_code(), SystemErrorCode::Unrecoverable);
  };

  check_concurrent_entry_bad(1);

  check_concurrent_entry(0, std::nullopt);

  check_concurrent_entry_bad(1);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(1));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(2));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(3));

  check_concurrent_entry(0, make_test_queue_entry(1));
  check_concurrent_entry(1, make_test_queue_entry(2));
  check_concurrent_entry_bad(3);
  check_concurrent_entry(2, make_test_queue_entry(3));
  check_concurrent_entry(3, std::nullopt);
  check_concurrent_entry_bad(4);
}

TEST(SnapshotStorageItemMakeVariant, RejectsUnsupportedWireTypes)
{
  // ContractData accepts value/queue/deleted/replay-cache; not auth or
  // deployed-contract-hash.
  EXPECT_TRUE(SnapshotStorageItem::make_variant(
                  WireStorageItem(StorageItemType::kItemTypeBytestring),
                  StorageKey(StorageKeyType::ContractData))
                  .has_value());
  EXPECT_FALSE(SnapshotStorageItem::make_variant(
                   WireStorageItem(StorageItemType::kItemTypeAuthData),
                   StorageKey(StorageKeyType::ContractData))
                   .has_value());
  EXPECT_FALSE(
      SnapshotStorageItem::make_variant(
          WireStorageItem(StorageItemType::kItemTypeDeployedContractHash),
          StorageKey(StorageKeyType::ContractData))
          .has_value());

  // FreeNativeTokenBalance accepts nnint/deleted only.
  EXPECT_TRUE(SnapshotStorageItem::make_variant(
                  WireStorageItem(StorageItemType::kItemTypeNonnegativeInt),
                  StorageKey(StorageKeyType::FreeNativeTokenBalance))
                  .has_value());
  EXPECT_FALSE(SnapshotStorageItem::make_variant(
                   WireStorageItem(StorageItemType::kItemTypeReplayCacheData),
                   StorageKey(StorageKeyType::FreeNativeTokenBalance))
                   .has_value());

  // AuthData accepts auth/deleted only.
  EXPECT_TRUE(SnapshotStorageItem::make_variant(
                  WireStorageItem(StorageItemType::kItemTypeAuthData),
                  StorageKey(StorageKeyType::AuthData))
                  .has_value());
  EXPECT_FALSE(SnapshotStorageItem::make_variant(
                   WireStorageItem(StorageItemType::kItemTypeBytestring),
                   StorageKey(StorageKeyType::AuthData))
                   .has_value());
}
} // namespace hogd
