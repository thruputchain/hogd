#include <gtest/gtest.h>

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hogexec/runtime/test_util/queue_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

class ItemViewTest : public ::testing::Test {
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

  std::unique_ptr<SnapshotStorageItem> make_empty_native_token_balance_item()
  {
    assert(db_proxy_);
    return std::make_unique<SnapshotStorageItem>(
        StorageItem(StorageItemType::kItemTypeDeleted),
        AddressAndKey{
            .address = ContractAddress{},
            .key = StorageKey(
                StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)},
        BlockID{1}, *db_proxy_);
  }

  std::unique_ptr<SnapshotStorageItem> make_rcq_item()
  {
    return std::make_unique<SnapshotStorageItem>(
        StorageItem(StorageItemType::kItemTypeRetrocausalQueue),
        AddressAndKey{}, BlockID{1}, *db_proxy_);
  }
};

TEST_F(ItemViewTest, Exists)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());
  ASSERT_FALSE(proxy.template get<BytestringItem>());

  Bytestring bytes{0, 1, 2, 3};
  const auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_FALSE(get_res);

  BytestringItem write1;
  write1.bytestring_ = bytes;

  EXPECT_TRUE(proxy.set(write1));

  ASSERT_TRUE(proxy.template get<BytestringItem>());
}

TEST_F(ItemViewTest, SetBytestring)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());

  Bytestring bytes{0, 1, 2, 3}, bytes2{3, 2, 1, 0},
      bytes_long(kBytestringMaxLen + 10, 0);
  EXPECT_TRUE(proxy.set(BytestringItem(bytes)));

  const auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->bytestring_, bytes);

  EXPECT_TRUE(proxy.set(BytestringItem(bytes2)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);

  EXPECT_FALSE(proxy.set(BytestringItem(bytes_long)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);
}

TEST_F(ItemViewTest, NNIntSetAddPositiveSet)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());

  ASSERT_FALSE(proxy.template get<NNIntItem>());
  EXPECT_TRUE(proxy.set(NNIntItem(100, 10)));

  const auto *get_res = proxy.template get<NNIntItem>();

  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->base_value_, 100);
  EXPECT_EQ(get_res->add_value_, 10);

  EXPECT_FALSE(proxy.template get<BytestringItem>());
}

TEST_F(ItemViewTest, NNIntSetAddValidation)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());
  ASSERT_FALSE(proxy.template get<NNIntItem>());

  EXPECT_TRUE(proxy.set(NNIntItem(100, -10)));
  EXPECT_FALSE(proxy.set(NNIntItem(100, -110)));
  EXPECT_TRUE(proxy.set(NNIntItem(100, -100)));

  EXPECT_TRUE(proxy.set(NNIntItem(-10, 1)));
  EXPECT_TRUE(proxy.set(NNIntItem(-10, 0)));
  EXPECT_FALSE(proxy.set(NNIntItem(-10, -1)));

  EXPECT_TRUE(proxy.set(NNIntItem(0, 0)));
  EXPECT_FALSE(proxy.set(NNIntItem(0, -1)));
}

TEST_F(ItemViewTest, RCQRead)
{
  auto item = make_rcq_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  // set up data in db to read
  auto queue = make_queue({
      {make_test_queue_entry(1)},
      {make_test_queue_entry(2), make_test_queue_entry(3)},

      {make_test_queue_entry(4), make_test_queue_entry(5)},

      {make_test_queue_entry(6)},
  });

  StorageCostModel cost_model(12000, 1, 5, 100, 5);
  server_.setup_create_item(AddressAndKey{},
                            StorageItemWrapper(std::move(queue), &cost_model));
  ExecutionModeData execution_mode_data = ProposalModeData();

  auto check_item = [&](std::optional<uint8_t> const &expected_entry,
                        bool advance_cursor) {
    auto r = rcq_item.get_next_item(execution_mode_data, advance_cursor);
    ASSERT_TRUE(r.has_value());
    if (expected_entry) {
      ASSERT_TRUE(r.value().has_value());
      EXPECT_EQ(r.value().value(),
                make_test_queue_entry(expected_entry.value()));
    }
    else {
      EXPECT_FALSE(r.value());
    }
  };

  check_item(1, false);
  check_item(1, false);
  check_item(1, false);
  check_item(1, true);
  check_item(2, false);
  check_item(2, false);
  check_item(2, true);
  check_item(3, false);
  check_item(3, true);
  check_item(4, true);
  check_item(5, false);
  check_item(5, true);
  check_item(6, true);
  check_item(std::nullopt, true);
  check_item(std::nullopt, true);

  // behavior is to read self entries before concurrent entries

  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(8));
  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(9));

  rcq_item.insert_entry(make_test_queue_entry(7));

  check_item(7, false);
  check_item(7, true);
  check_item(8, false);
  check_item(8, true);
  check_item(9, false);

  rcq_item.insert_entry(make_test_queue_entry(10));

  check_item(10, false);
  check_item(10, true);

  check_item(9, false);
  check_item(9, true);

  check_item(std::nullopt, true);
  check_item(std::nullopt, true);

  server_.setup_add_concurrent_rcq_item(AddressAndKey{},
                                        make_test_queue_entry(11));

  check_item(11, false);
  check_item(11, true);
  check_item(std::nullopt, true);
  check_item(std::nullopt, true);

  // Check that we get the exact same pattern during validation
  ExecutionModeData validation_mode_data = ExecutionModeData(
      std::in_place_type_t<ValidationModeData>(),
      std::get<ProposalModeData>(execution_mode_data).proposal_data());

  auto validate_rcq_item = RetrocausalQueueItem(*item);

  auto check_item_validation = [&](std::optional<uint8_t> const &expected_entry,
                                   bool advance_cursor) {
    auto r =
        validate_rcq_item.get_next_item(validation_mode_data, advance_cursor);
    ASSERT_TRUE(r.has_value());
    if (expected_entry) {
      ASSERT_TRUE(r.value().has_value());
      EXPECT_EQ(r.value().value(),
                make_test_queue_entry(expected_entry.value()));
    }
    else {
      EXPECT_FALSE(r.value());
    }
  };

  // exact same sequence of read and write operations as above
  // but in validation mode, so we should get the exact same result

  check_item_validation(1, false);
  check_item_validation(1, false);
  check_item_validation(1, false);
  check_item_validation(1, true);
  check_item_validation(2, false);
  check_item_validation(2, false);
  check_item_validation(2, true);
  check_item_validation(3, false);
  check_item_validation(3, true);
  check_item_validation(4, true);
  check_item_validation(5, false);
  check_item_validation(5, true);
  check_item_validation(6, true);
  check_item_validation(std::nullopt, true);
  check_item_validation(std::nullopt, true);

  validate_rcq_item.insert_entry(make_test_queue_entry(7));

  check_item_validation(7, false);
  check_item_validation(7, true);
  check_item_validation(8, false);
  check_item_validation(8, true);
  check_item_validation(9, false);

  validate_rcq_item.insert_entry(make_test_queue_entry(10));

  check_item_validation(10, false);
  check_item_validation(10, true);

  check_item_validation(9, false);
  check_item_validation(9, true);

  check_item_validation(std::nullopt, true);
  check_item_validation(std::nullopt, true);

  check_item_validation(11, false);
  check_item_validation(11, true);
  check_item_validation(std::nullopt, true);
  check_item_validation(std::nullopt, true);

  EXPECT_TRUE(
      std::get<ValidationModeData>(validation_mode_data).check_all_consumed());
}

TEST_F(ItemViewTest, NativeTokenBalance)
{
  auto item = make_empty_native_token_balance_item();
  ItemView proxy(item.get());

  ASSERT_FALSE(proxy.template get<NativeTokenBalanceItem>());

  NativeTokenBalanceItem write(100);
  EXPECT_TRUE(proxy.set(write));
  EXPECT_TRUE(proxy.template get<NativeTokenBalanceItem>());

  write.add_deterministic_native_token_balance(200);
  EXPECT_TRUE(proxy.set(write));

  write.add_deterministic_native_token_balance(-400);
  EXPECT_FALSE(proxy.set(write));

  EXPECT_EQ(write.get_user_visible_balance(), -100);

  // return to a positive deterministic balance, and now
  // add a nondeterministic sub amount
  write.add_deterministic_native_token_balance(1000);

  // the amounts returned from the db are positive;
  // these will be subtracted from the user's balance
  write.add_nondeterministic_native_token_lock(
      Nondeterministic<int128_t>(1000));
  EXPECT_TRUE(proxy.set(write));

  // the nondeterministic sub amount is not visible to the user
  EXPECT_EQ(write.get_user_visible_balance(), 900);

  auto delta = proxy.get_delta();
  ASSERT_TRUE(delta.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(delta->type(), StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
  EXPECT_EQ(from_xdr(delta->nnint().set_value), 100);
  EXPECT_EQ(from_xdr(delta->nnint().add_value), -200);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

} // namespace hogd
