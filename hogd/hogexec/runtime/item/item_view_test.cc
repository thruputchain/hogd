#include <gtest/gtest.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hogexec/runtime/test_util/queue_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class ItemViewTest : public ::testing::Test {
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

  std::unique_ptr<SnapshotStorageItem> make_empty_native_token_balance_item()
  {
    assert(db_proxy_);
    AddressAndKey addrkey{.address = ContractAddress{},
                          .key = StorageKey(
                              StorageKeyType::FreeNativeTokenBalance)};
    auto entry = SnapshotStorageItem::make_variant(
        WireStorageItem(StorageItemType::kItemTypeDeleted), addrkey.key);
    CHECK(entry.has_value());
    return std::make_unique<SnapshotStorageItem>(std::move(entry).value(),
                                                 std::move(addrkey), BlockID{1},
                                                 *db_proxy_);
  }

  std::unique_ptr<SnapshotStorageItem> make_rcq_item()
  {
    AddressAndKey addrkey{};
    auto entry = SnapshotStorageItem::make_variant(
        WireStorageItem(StorageItemType::kItemTypeRetrocausalQueue),
        addrkey.key);
    CHECK(entry.has_value());
    return std::make_unique<SnapshotStorageItem>(std::move(entry).value(),
                                                 std::move(addrkey), BlockID{1},
                                                 *db_proxy_);
  }

  static void check_eq(QueueEntry const &value, uint32_t expected)
  {
    ASSERT_EQ(value.size(), sizeof(expected));
    ASSERT_EQ(read_entry(value), expected);
  }

  static uint32_t read_entry(QueueEntry const &value)
  {
    assert(value.size() == sizeof(uint32_t));
    uint32_t entry;
    std::memcpy(&entry, value.data(), sizeof(entry));
    return entry;
  }

  template<size_t MaxSize>
  static std::vector<uint32_t>
  read_entries(x23::Vec<QueueEntry, MaxSize> const &entries)
  {
    std::vector<uint32_t> out;
    out.reserve(entries.size());
    for (auto const &entry : entries) {
      out.push_back(read_entry(entry));
    }
    // get_delta() shuffles concurrent consumes in debug builds,
    // so sort for deterministic comparison.
    std::ranges::sort(out);
    return out;
  }
};

TEST_F(ItemViewTest, Exists)
{
  ItemView proxy;
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
  ItemView proxy;

  Bytestring bytes{0, 1, 2, 3}, bytes2{3, 2, 1, 0};
  EXPECT_TRUE(proxy.set(BytestringItem(bytes)));

  const auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->bytestring_, bytes);

  EXPECT_TRUE(proxy.set(BytestringItem(bytes2)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);
}

TEST_F(ItemViewTest, NNIntSetAddPositiveSet)
{
  ItemView proxy;

  ASSERT_FALSE(proxy.template get<NNIntItem>());
  EXPECT_TRUE(
      proxy.set(NNIntItem(to_xdr(int128_t(100)), to_xdr(int128_t(10)))));

  const auto *get_res = proxy.template get<NNIntItem>();

  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->base_value_, to_xdr(int128_t(100)));
  EXPECT_EQ(get_res->add_value_, to_xdr(int128_t(10)));

  EXPECT_FALSE(proxy.template get<BytestringItem>());
}

TEST_F(ItemViewTest, NNIntSetAddValidation)
{
  ItemView proxy;
  ASSERT_FALSE(proxy.template get<NNIntItem>());

  EXPECT_TRUE(
      proxy.set(NNIntItem(to_xdr(int128_t(100)), to_xdr(int128_t(-10)))));
  EXPECT_FALSE(
      proxy.set(NNIntItem(to_xdr(int128_t(100)), to_xdr(int128_t(-110)))));
  EXPECT_TRUE(
      proxy.set(NNIntItem(to_xdr(int128_t(100)), to_xdr(int128_t(-100)))));

  EXPECT_TRUE(proxy.set(NNIntItem(to_xdr(int128_t(-10)), to_xdr(int128_t(1)))));
  EXPECT_TRUE(proxy.set(NNIntItem(to_xdr(int128_t(-10)), to_xdr(int128_t(0)))));
  EXPECT_FALSE(
      proxy.set(NNIntItem(to_xdr(int128_t(-10)), to_xdr(int128_t(-1)))));

  EXPECT_TRUE(proxy.set(NNIntItem(to_xdr(int128_t(0)), to_xdr(int128_t(0)))));
  EXPECT_FALSE(proxy.set(NNIntItem(to_xdr(int128_t(0)), to_xdr(int128_t(-1)))));
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

  StorageCostModel cost_model(12000, 1, 5, 5, 0);
  server_->impl()->setup_create_item(AddressAndKey{},
                                     StorageItemWrapper(std::move(queue),
                                                        &cost_model));
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

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(8));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(9));

  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(7)).has_value());

  check_item(7, false);
  check_item(7, true);
  check_item(8, false);
  check_item(8, true);
  check_item(9, false);

  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(10)).has_value());

  check_item(10, false);
  check_item(10, true);

  check_item(9, false);
  check_item(9, true);

  check_item(std::nullopt, true);
  check_item(std::nullopt, true);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
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

  ASSERT_TRUE(
      validate_rcq_item.insert_entry(make_test_queue_entry(7)).has_value());

  check_item_validation(7, false);
  check_item_validation(7, true);
  check_item_validation(8, false);
  check_item_validation(8, true);
  check_item_validation(9, false);

  ASSERT_TRUE(
      validate_rcq_item.insert_entry(make_test_queue_entry(10)).has_value());

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

TEST_F(ItemViewTest, CrossTypeOverwriteNNIntToBytestring)
{
  ItemView proxy;

  // Write an NNInt item
  EXPECT_TRUE(proxy.set(NNIntItem(to_xdr(int128_t(42)), to_xdr(int128_t(10)))));
  ASSERT_TRUE(proxy.template get<NNIntItem>());
  EXPECT_EQ(proxy.template get<NNIntItem>()->base_value_, to_xdr(int128_t(42)));

  // Overwrite with a BytestringItem on the same ItemView
  Bytestring bytes{0xDE, 0xAD, 0xBE, 0xEF};
  EXPECT_TRUE(proxy.set(BytestringItem(bytes)));

  // After overwrite, NNIntItem should be gone
  ASSERT_EQ(proxy.template get<NNIntItem>(), nullptr);

  // BytestringItem should be present with the new value
  const auto *bs = proxy.template get<BytestringItem>();
  ASSERT_TRUE(bs);
  EXPECT_EQ(bs->bytestring_, bytes);

  // get_delta should produce a bytestring delta (not NNInt)
  auto [delta, flags] = proxy.get_delta();
  ASSERT_TRUE(delta.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(delta->type(), StorageDeltaType::kDeltaTypeBytestringSet);
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_F(ItemViewTest, RCQInsertOverflow)
{
  auto item = make_rcq_item();
  auto rcq_item = RetrocausalQueueItem(*item);
  for (int i = 0; i < kRetrocausalQueueBlockEntriesMaxLen; i++) {
    ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(i)).has_value());
  }
  ASSERT_FALSE(rcq_item.insert_entry(make_test_queue_entry(0)).has_value());

  // Now check the ordering
  ExecutionModeData mode = ProposalModeData();

  for (int i = 0; i < kRetrocausalQueueBlockEntriesMaxLen; i++) {
    auto res = rcq_item.get_next_item(mode, true);
    ASSERT_TRUE(res.has_value() && res.value().has_value());
    auto const &val = res.value().value();
    check_eq(val, i);
  }
  auto res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && !res.value().has_value());
}

TEST_F(ItemViewTest, CurrentBlockConsumeOrder)
{
  auto item = make_rcq_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(1)).has_value());
  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(2)).has_value());
  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(3)).has_value());

  ExecutionModeData mode = ProposalModeData();

  auto check_read = [&](uint8_t expect, bool advance_cursor) {
    auto res = rcq_item.get_next_item(mode, advance_cursor);
    ASSERT_TRUE(res.has_value() && res.value().has_value());
    auto const &val = res.value().value();
    check_eq(val, expect);
  };

  check_read(1, false);

  check_read(1, true);
  check_read(2, true);

  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(4)).has_value());
  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(5)).has_value());
  check_read(3, true);
  check_read(4, false);
  check_read(4, true);
  check_read(5, true);
}

TEST_F(ItemViewTest, CurrentBlockInsertConsumeSelfDelta)
{
  auto item = make_rcq_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(1)).has_value());
  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(2)).has_value());
  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(3)).has_value());

  ExecutionModeData mode = ProposalModeData();

  auto check_read = [&](uint8_t expect, bool advance_cursor) {
    auto res = rcq_item.get_next_item(mode, advance_cursor);
    ASSERT_TRUE(res.has_value() && res.value().has_value());
    auto const &val = res.value().value();
    check_eq(val, expect);
  };

  check_read(1, false);

  auto delta = rcq_item.get_delta();
  ASSERT_EQ(delta.type(), StorageDeltaType::kDeltaTypeRetrocausalQueue);
  ASSERT_FALSE(delta.retrocausal_queue().consume);
  ASSERT_EQ(delta.retrocausal_queue().insert.added_entries.size(), 3);

  check_read(1, true);

  delta = rcq_item.get_delta();
  ASSERT_EQ(delta.type(), StorageDeltaType::kDeltaTypeRetrocausalQueue);
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  ASSERT_EQ(delta.retrocausal_queue().consume->current_block_consumed.size(),
            0);
  ASSERT_EQ(delta.retrocausal_queue().insert.added_entries.size(), 2);
  check_eq(delta.retrocausal_queue().insert.added_entries[0], 2);
  check_eq(delta.retrocausal_queue().insert.added_entries[1], 3);

  check_read(2, true);

  delta = rcq_item.get_delta();
  ASSERT_EQ(delta.type(), StorageDeltaType::kDeltaTypeRetrocausalQueue);
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  ASSERT_EQ(delta.retrocausal_queue().consume->current_block_consumed.size(),
            0);
  ASSERT_EQ(delta.retrocausal_queue().insert.added_entries.size(), 1);
  check_eq(delta.retrocausal_queue().insert.added_entries[0], 3);

  check_read(3, true);

  delta = rcq_item.get_delta();
  ASSERT_EQ(delta.type(), StorageDeltaType::kDeltaTypeRetrocausalQueue);
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  ASSERT_EQ(delta.retrocausal_queue().consume->current_block_consumed.size(),
            0);
  ASSERT_EQ(delta.retrocausal_queue().insert.added_entries.size(), 0);
}

TEST_F(ItemViewTest, PastBlockConsumeDeltaTracksBlockBoundaries)
{
  auto item = make_rcq_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  auto queue = make_queue({
      {make_test_queue_entry(1), make_test_queue_entry(2)},
      {make_test_queue_entry(3), make_test_queue_entry(4)},
  });

  StorageCostModel cost_model(12000, 1, 5, 5, 0);
  server_->impl()->setup_create_item(AddressAndKey{},
                                     StorageItemWrapper(std::move(queue),
                                                        &cost_model));

  ExecutionModeData mode = ProposalModeData();

  auto res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 1);

  auto delta = rcq_item.get_delta();
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.max_fully_consumed_block_number,
            0);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.partial_consumed_number,
            1);
  EXPECT_TRUE(
      delta.retrocausal_queue().consume->current_block_consumed.empty());
  EXPECT_TRUE(delta.retrocausal_queue().insert.added_entries.empty());

  res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 2);

  delta = rcq_item.get_delta();
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.max_fully_consumed_block_number,
            1);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.partial_consumed_number,
            0);

  res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 3);

  delta = rcq_item.get_delta();
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.max_fully_consumed_block_number,
            1);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.partial_consumed_number,
            1);
  EXPECT_TRUE(
      delta.retrocausal_queue().consume->current_block_consumed.empty());
}

TEST_F(ItemViewTest, CurrentBlockConcurrentConsumeAppearsInDelta)
{
  auto item = make_empty_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(7));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(8));

  ExecutionModeData mode = ProposalModeData();

  auto res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 7);

  auto delta = rcq_item.get_delta();
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.max_fully_consumed_block_number,
            0);
  EXPECT_EQ(delta.retrocausal_queue()
                .consume->past_block_consume.partial_consumed_number,
            0);
  EXPECT_EQ(read_entries(
                delta.retrocausal_queue().consume->current_block_consumed),
            (std::vector<uint32_t>{7}));
  EXPECT_TRUE(delta.retrocausal_queue().insert.added_entries.empty());

  res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 8);

  delta = rcq_item.get_delta();
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(read_entries(
                delta.retrocausal_queue().consume->current_block_consumed),
            (std::vector<uint32_t>{7, 8}));
  EXPECT_TRUE(delta.retrocausal_queue().insert.added_entries.empty());
}

TEST_F(ItemViewTest,
       MixedCurrentBlockDeltaKeepsConcurrentConsumesAndLiveSelfInserts)
{
  auto item = make_empty_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(20));

  ExecutionModeData mode = ProposalModeData();

  auto res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 20);

  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(1)).has_value());
  ASSERT_TRUE(rcq_item.insert_entry(make_test_queue_entry(2)).has_value());

  res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 1);

  auto delta = rcq_item.get_delta();
  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(read_entries(
                delta.retrocausal_queue().consume->current_block_consumed),
            (std::vector<uint32_t>{20}));
  ASSERT_EQ(delta.retrocausal_queue().insert.added_entries.size(), 1);
  check_eq(delta.retrocausal_queue().insert.added_entries[0], 2);
}

TEST_F(ItemViewTest, MakeFullyConsumedItemPreservesConcurrentConsumeDelta)
{
  auto item = make_empty_item();
  auto rcq_item = RetrocausalQueueItem(*item);

  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(42));
  server_->impl()->setup_add_concurrent_rcq_item(AddressAndKey{},
                                                 make_test_queue_entry(99));

  ExecutionModeData mode = ProposalModeData();

  auto res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 42);

  res = rcq_item.get_next_item(mode, true);
  ASSERT_TRUE(res.has_value() && res.value().has_value());
  check_eq(res.value().value(), 99);

  auto fully_consumed = RetrocausalQueueItem::make_fully_consumed_item(
      *item, item->get_retrocausally_consumed_entries(
                 rcq_item.get_retrocausally_consumed_count()));
  auto delta = fully_consumed.get_delta();

  ASSERT_TRUE(delta.retrocausal_queue().consume);
  EXPECT_EQ(read_entries(
                delta.retrocausal_queue().consume->current_block_consumed),
            (std::vector<uint32_t>{42, 99}));
  EXPECT_TRUE(delta.retrocausal_queue().insert.added_entries.empty());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
