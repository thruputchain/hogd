#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)
class DirectRCQTests : public ::testing::Test,
                       public NativeContractTestFramework {
public:
  DirectRCQTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  void SetUp() override { cost_model_ = get_default_cost_model(); }

  StorageCostModel const *cost_model_;

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }

  constexpr static auto make_rcq_entry(std::vector<uint8_t> const &values)
  {
    return QueueEntry{values.begin(), values.end()};
  }

  constexpr static auto
  make_rcq_insert_delta(std::vector<QueueEntry> const &values)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{
        .added_entries = {values.begin(), values.end()}};
    return delta;
  }

  constexpr static auto make_rcq_consume_delta(
      uint32_t max_fully_consumed_block_number,
      uint32_t partial_consumed_number,
      x23::Vec<QueueEntry, 0xffffffff> const &current_block_consumed)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    RetrocausalQueuePastBlockConsumeDelta consume_delta = {
        .max_fully_consumed_block_number = max_fully_consumed_block_number,
        .partial_consumed_number = partial_consumed_number};
    delta.retrocausal_queue().consume =
        RetrocausalQueueConsumeDelta{.past_block_consume = consume_delta,
                                     .current_block_consumed =
                                         current_block_consumed};
    return delta;
  }

  constexpr static auto make_rcq_consume_insert_delta(
      uint32_t max_fully_consumed_block_number,
      uint32_t partial_consumed_number,
      x23::Vec<QueueEntry, 0xffffffff> const &current_block_consumed,
      std::vector<QueueEntry> const &inserted_entries)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    RetrocausalQueuePastBlockConsumeDelta consume_delta = {
        .max_fully_consumed_block_number = max_fully_consumed_block_number,
        .partial_consumed_number = partial_consumed_number};
    delta.retrocausal_queue().consume =
        RetrocausalQueueConsumeDelta{.past_block_consume = consume_delta,
                                     .current_block_consumed =
                                         current_block_consumed};
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{
        .added_entries = {inserted_entries.begin(), inserted_entries.end()}};
    return delta;
  }

  std::optional<QueueEntry> load_concurrent_entry(AddressAndKey const &addrkey,
                                                  uint32_t index)
  {
    auto &proxy = get_db_proxy();
    auto res = proxy.get_concurrent_retrocausal_queue_entry(get_block_number(),
                                                            addrkey, index);
    if (!res) {
      panic("Failed to load concurrent entry");
    }
    return res.value();
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);
};

TEST_F(DirectRCQTests, ConcurrentInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConcurrentConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto entry1 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  ASSERT_FALSE(entry1.has_value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  entry1 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress,
                                                            "rcq0"),
                                 0);
  ASSERT_TRUE(entry1.has_value());
  ASSERT_EQ(entry1.value(), make_rcq_entry({0}));

  auto entry2 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 1);
  ASSERT_FALSE(entry2.has_value());

  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  entry2 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress,
                                                            "rcq0"),
                                 1);
  ASSERT_TRUE(entry2.has_value());
  ASSERT_EQ(entry2.value(), make_rcq_entry({1}));
}

TEST_F(DirectRCQTests, ConcurrentConsumesDisallowed)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // This does not arise when proposing a new block but could arise when
  // validating another node's proposal
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value()); // double consume fails

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConcurrentConsumesDisallowed2)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // This does not arise when proposing a new block but could arise when
  // validating another node's proposal
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value()); // double consume fails
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // second consume can be reserved if the first is aborted
  auto tx4 = make_new_per_tx_db_proxy();
  tx4.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx4.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConsumeExclusiveLock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // then upgrade tx1 to exclusive lock by changing the delta type
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // another tx cannot concurrently insert
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // Another tx cannot concurrently make a bytestring write
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // tx1 can revert back to treating the object as an rcq
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // Now we can retrieve the retrocausal entry
  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  ASSERT_TRUE(entry.has_value());
  ASSERT_EQ(entry.value(), make_rcq_entry({1}));

  auto entry2 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 1);
  ASSERT_FALSE(entry2.has_value());
}

TEST_F(DirectRCQTests, RevertFromExclusiveLock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // then upgrade tx1 to exclusive lock by changing the delta type
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // concurrent tx cannot insert right now
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // tx1 can revert back to treating the object as an rcq
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // concurrent tx can insert right now
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());

  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  ASSERT_TRUE(entry.has_value());
  ASSERT_EQ(entry.value(), make_rcq_entry({0}));
}

TEST_F(DirectRCQTests, OverwrittenDeltasNotMadeVisible)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  EXPECT_FALSE(entry.has_value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  entry = load_concurrent_entry(make_contract_data_addrkey(kContractAddress,
                                                           "rcq0"),
                                0);
  EXPECT_FALSE(entry.has_value());
}

TEST_F(DirectRCQTests, ChangeTypeAndOverwriteInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  EXPECT_FALSE(entry.has_value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  entry = load_concurrent_entry(make_contract_data_addrkey(kContractAddress,
                                                           "rcq0"),
                                0);
  EXPECT_FALSE(entry.has_value());
}

TEST_F(DirectRCQTests, InsertConflictsWithDelete)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  EXPECT_FALSE(entry.has_value());
}

// check the conflict in the other order from InsertConflictsWithDelete
TEST_F(DirectRCQTests, InsertConflictsWithDelete2)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());

  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  EXPECT_FALSE(entry.has_value());

  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx1.abort_transaction().has_value());

  entry = load_concurrent_entry(make_contract_data_addrkey(kContractAddress,
                                                           "rcq0"),
                                0);
  EXPECT_TRUE(entry.has_value());
}

// Verifies MPSC semantics: after one tx takes the consume lock, other txs
// can still insert to the same RCQ within the same block.
TEST_F(DirectRCQTests, ConcurrentInsertAndConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  // tx1 inserts an entry
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // tx2 consumes — takes the single consumer slot
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // tx3 inserts a different entry — should succeed despite active consumer
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}

// Verifies that a single insert delta can contain multiple entries and that
// multiple concurrent transactions can each batch-insert to the same RCQ.
TEST_F(DirectRCQTests, MultipleEntriesPerInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  // tx1 inserts three entries in a single delta
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0}), make_rcq_entry({1}),
                                       make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // tx2 concurrently inserts two entries in a single delta
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta(
                    {make_rcq_entry({3}), make_rcq_entry({4})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  auto entry = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
  ASSERT_TRUE(entry.has_value());
  ASSERT_EQ(entry.value(), make_rcq_entry({0}));

  auto entry2 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 1);
  ASSERT_TRUE(entry2.has_value());
  ASSERT_EQ(entry2.value(), make_rcq_entry({1}));

  auto entry3 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 2);
  ASSERT_TRUE(entry3.has_value());
  ASSERT_EQ(entry3.value(), make_rcq_entry({2}));

  auto entry4 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 3);
  ASSERT_TRUE(entry4.has_value());
  ASSERT_EQ(entry4.value(), make_rcq_entry({3}));

  auto entry5 = load_concurrent_entry(
      make_contract_data_addrkey(kContractAddress, "rcq0"), 4);
  ASSERT_TRUE(entry5.has_value());
  ASSERT_EQ(entry5.value(), make_rcq_entry({4}));
}

TEST_F(DirectRCQTests, ConsumeAndInsertNetZeroDoesNotPersistEmptyBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq0");

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({7})},
                                             {make_rcq_entry({7})}),
               std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx.commit_transaction({}).has_value());

  advance_block();

  auto queue = lookup_storage_item_from_db(key);
  ASSERT_FALSE(queue.has_value());

  auto next_block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(next_block.has_value());
  ASSERT_FALSE(next_block.value().has_value());
}

// This test probes block-number lookup behavior in a way that is currently
// under active interpretation. Keep disabled while we move coverage to the
// dedicated RCQ read semantics suite.
TEST_F(DirectRCQTests, PersistedBlocksRemainContiguousAfterPruneAndInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq0");

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta({make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block();

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {}, {make_rcq_entry({3})}),
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
  advance_block();

  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 1);
  ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number, 0);
  ASSERT_EQ(queue->metadata().max_live_block_num, 3);
  ASSERT_EQ(queue->metadata().live_entries, 2);

  // This test sets search_start_number to 2, instead of 1 (the fully consumed
  // rcq block) so that this test passes without triggering the diff between
  // hogdb and mockdb.
  auto b2 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 2);
  ASSERT_TRUE(b2.has_value());
  ASSERT_TRUE(b2.value().has_value());
  ASSERT_EQ(b2.value()->block_number, 2);
  ASSERT_EQ(b2.value()->entries.size(), 1);
  ASSERT_EQ(b2.value()->entries[0], make_rcq_entry({2}));

  auto b3 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 3);
  ASSERT_TRUE(b3.has_value());
  ASSERT_TRUE(b3.value().has_value());
  ASSERT_EQ(b3.value()->block_number, 3);
  ASSERT_EQ(b3.value()->entries.size(), 1);
  ASSERT_EQ(b3.value()->entries[0], make_rcq_entry({3}));

  auto b4 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 4);
  ASSERT_TRUE(b4.has_value());
  ASSERT_FALSE(b4.value().has_value());
}

TEST_F(DirectRCQTests, ConsumeSingleBlockOneByOneAcrossBlocks)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq0");

  std::vector<QueueEntry> inserted = {make_rcq_entry({0}), make_rcq_entry({1}),
                                      make_rcq_entry({2}), make_rcq_entry({3})};

  auto tx_insert = make_new_per_tx_db_proxy();
  tx_insert.add_delta(key, make_rcq_insert_delta(inserted),
                      std::to_underlying(
                          StorageDeltaFlags::kStorageDeltaFlagNone));
  auto r = tx_insert.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_insert.commit_transaction({}).has_value());
  advance_block();

  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 0);
  ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number, 0);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);
  ASSERT_EQ(queue->metadata().live_entries, inserted.size());

  for (uint32_t consumed = 1; consumed < inserted.size(); ++consumed) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(key, make_rcq_consume_delta(0, consumed, {}),
                 std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx.commit_transaction({}).has_value());
    advance_block();

    queue = lookup_storage_item_from_db(key);
    ASSERT_TRUE(queue.has_value());
    ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
    ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number,
              0);
    ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number,
              consumed);
    ASSERT_EQ(queue->metadata().max_live_block_num, 1);
    ASSERT_EQ(queue->metadata().live_entries, inserted.size() - consumed);

    auto block =
        get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                        1);
    ASSERT_TRUE(block.has_value());
    ASSERT_TRUE(block.value().has_value());
    ASSERT_EQ(block.value()->block_number, 1);
    ASSERT_EQ(block.value()->entries.size(), inserted.size());
  }

  // Consume the final entry and fully clear block 1.
  auto tx_final = make_new_per_tx_db_proxy();
  tx_final.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                     std::to_underlying(
                         StorageDeltaFlags::kStorageDeltaFlagNone));
  r = tx_final.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_final.commit_transaction({}).has_value());
  advance_block();

  auto final_queue = lookup_storage_item_from_db(key);
  ASSERT_FALSE(final_queue.has_value());

  auto block_after_clear =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block_after_clear.has_value());
  ASSERT_FALSE(block_after_clear.value().has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)
} // namespace hogd
