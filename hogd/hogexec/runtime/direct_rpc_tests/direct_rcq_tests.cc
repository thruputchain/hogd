#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
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

TEST_F(DirectRCQTests, ValidationFailsThenPropose)
{
  auto handler = make_new_per_tx_db_proxy();
  handler.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                    make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                    std::to_underlying(StorageDeltaFlags::None));
  auto r = handler.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  // TX must be committed so the lock is finalized and
  // prepare_committed_changes runs the consumed_set check.
  ASSERT_TRUE(handler.commit_transaction({}).has_value());

  reject_block(BlockExecutionMode::kBlockExecutionModeProposal);
  advance_block();
}

TEST_F(DirectRCQTests, ConcurrentInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConcurrentInsert2)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
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

TEST_F(DirectRCQTests, ConcurrentEmptyConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  // second consume fails
  ASSERT_TRUE(r.has_value() && !r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConcurrentConsumeSelf)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  // This, on its own, would be bad input from hogexec.
  // However, it's not clear that this can be easily distinguished
  // (looking just at the transaction) from the case of a concurrent insert of
  // {0} from a separate transaction, which was then consumed and reinserted by
  // this tx. We therefore allow this case to simplify the database
  // implementation.

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({0})},
                                              {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  advance_block();
}

TEST_F(DirectRCQTests, ConcurrentConsumesDisallowed)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // This does not arise when proposing a new block but could arise when
  // validating another node's proposal
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // This does not arise when proposing a new block but could arise when
  // validating another node's proposal
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value()); // double consume fails
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // second consume can be reserved if the first is aborted
  auto tx4 = make_new_per_tx_db_proxy();
  tx4.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // then upgrade tx1 to exclusive lock by changing the delta type
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // another tx cannot concurrently insert
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // Another tx cannot concurrently make a bytestring write
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
  ASSERT_TRUE(tx3.abort_transaction().has_value());

  // tx1 can revert back to treating the object as an rcq
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // then upgrade tx1 to exclusive lock by changing the delta type
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // concurrent tx cannot insert right now
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value());
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // tx1 can revert back to treating the object as an rcq
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // concurrent tx can insert right now
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_bytestring_delta("value"),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_delete_delta(),
                std::to_underlying(StorageDeltaFlags::None));
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // tx2 consumes — takes the single consumer slot
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // tx3 inserts a different entry — should succeed despite active consumer
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}

// Consume/insert contention:
// tx1 insert (current block), tx2 retrocausally consume, tx3 insert.
// This exercises lock accounting when entry count can both decrease (consume)
// and increase (inserts) within the same block.
TEST_F(DirectRCQTests, ConcurrentInsertRetroconsumeInsert)
{
  constexpr int128_t kInitialBalance = 1'000'000'000;
  setup_grant_storage_token(kContractAddress, kInitialBalance);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_contention");

  // Same block:
  // tx1 inserts, tx2 consumes block 1, tx3 inserts.
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(0, 0, {make_rcq_entry({10})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_insert_delta({make_rcq_entry({11})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Commit consume before inserts to stress reservation/release ordering.
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
  advance_block();

  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  // tx2's past_block_consume = {0, 0}: no past blocks consumed.
  // current_block_consumed = {10}: one entry consumed from current block.
  // Only block 1 is created (with remaining entries after filtering consumed).
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 0);
  ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number, 0);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);
  // 2 inserts ({10}, {11}), 1 current-block consume ({10}): live = 1
  ASSERT_EQ(queue->metadata().live_entries, 1);

  auto b1 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 1);
  ASSERT_TRUE(b1.has_value());
  ASSERT_TRUE(b1.value().has_value());
  ASSERT_EQ(b1.value()->block_number, 1);
  ASSERT_EQ(b1.value()->entries.size(), 1);
  ASSERT_EQ(b1.value()->entries[0], make_rcq_entry({11}));

  auto bal_item = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kContractAddress));
  ASSERT_TRUE(bal_item.has_value());
  ASSERT_EQ(from_xdr(bal_item->nnint()),
            kInitialBalance - cost_model_->get_retrocausal_queue_cost(
                                  /*active_queue_size=*/1));
}

// Repro harness for investigating HogDB invariant:
// assert(cost_change <= vec.back().locked_storage_cost) in invoke_tx.cc.
TEST_F(DirectRCQTests, ReproInvokeTxLockedStorageAssertOnConcurrentRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_assert_repro");

  auto submit_single = [&](StorageDelta delta) {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(key, std::move(delta),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx.commit_transaction({}).has_value());
    advance_block();
  };

  // Prepare two past blocks.
  submit_single(make_rcq_insert_delta({make_rcq_entry({1})}));
  submit_single(make_rcq_insert_delta({make_rcq_entry({2})}));

  // Same block: one tx fully consumes past blocks, one tx inserts.
  auto tx_consume = make_new_per_tx_db_proxy();
  tx_consume.add_delta(key, make_rcq_consume_delta(2, 0, {}),
                       std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_consume.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx_insert = make_new_per_tx_db_proxy();
  tx_insert.add_delta(key, make_rcq_insert_delta({make_rcq_entry({9})}),
                      std::to_underlying(StorageDeltaFlags::None));
  r = tx_insert.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // If the invariant fails, one of these commits will abort the process.
  ASSERT_TRUE(tx_consume.commit_transaction({}).has_value());
  ASSERT_TRUE(tx_insert.commit_transaction({}).has_value());
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
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // tx2 concurrently inserts two entries in a single delta
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"),
                make_rcq_insert_delta(
                    {make_rcq_entry({3}), make_rcq_entry({4})}),
                std::to_underlying(StorageDeltaFlags::None));
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
               std::to_underlying(StorageDeltaFlags::None));
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

TEST_F(DirectRCQTests, PersistedBlocksRemainContiguousAfterPruneAndInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq0");

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();

  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta({make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block();

  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {}, {make_rcq_entry({3})}),
                std::to_underlying(StorageDeltaFlags::None));
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

  auto b1 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 1);
  ASSERT_TRUE(b1.has_value());
  ASSERT_FALSE(b1.value().has_value());

  // RCQ block 1 is gone
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
                      std::to_underlying(StorageDeltaFlags::None));
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
                 std::to_underlying(StorageDeltaFlags::None));
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
                     std::to_underlying(StorageDeltaFlags::None));
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

// Stress_PopSubtransactionWithRCQInsert
//
// Push two subtransactions for the same RCQ key with cumulative inserts,
// then pop the deeper one. During rollback, InvokeTX::rollback_to_savepoint
// calls modify_lock(deep_change, shallow_change). In
// try_atomic_change_lock the CHECK at line 111 requires
// after.entries.size() >= before.entries.size(). Rolling back from {0,1} to
// {0} means 1 >= 2, which is false -> CHECK crash.
TEST_F(DirectRCQTests, StressPopSubtransactionWithRCQInsert)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_pop_sub");

  auto tx = make_new_per_tx_db_proxy();

  // Subtransaction 1: insert one entry.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Subtransaction 2: insert two entries (cumulative, includes entry 0).
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({0}), make_rcq_entry({1})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Pop the deeper subtransaction — this triggers rollback_to_savepoint
  // which calls modify_lock with (deep=2entries, shallow=1entry).
  auto pop_r = tx.pop_subtransactions(1);
  ASSERT_TRUE(pop_r.has_value()) << "pop_subtransactions returned error";

  // If we get here, commit and verify the earlier insert is intact.
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
}

// Stress_SameTxNonCumulativeRCQInserts
//
// Push subtransaction 1 with insert({0}), then subtransaction 2 with
// insert({1}) for the SAME key. The second push calls request_change which
// calls narrow (checks entries.size() >= earlier.size(), passes because
// 1 >= 1) and then try_atomic_change_lock which CHECKs that old entries
// are a prefix of new entries. {0} != {1} -> CHECK crash at line 104.
TEST_F(DirectRCQTests, StressSameTxNonCumulativeRCQInserts)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_noncumulative");

  auto tx = make_new_per_tx_db_proxy();

  // Subtransaction 1: insert entry {0}.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Subtransaction 2: insert entry {1} (NOT cumulative — omits {0}).
  // This should trigger CHECK(std::equal(...)) at rcq_lock.cc:104.
  // This is invalid input from the executor
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value());
}

TEST_F(DirectRCQTests, RollbackInsertions)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_rollback_insertions");

  auto tx = make_new_per_tx_db_proxy();

  // Subtransaction 1: insert entry {0}.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Subtransaction 2: insert entries {1, 2}.
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({0}), make_rcq_entry({1})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.pop_subtransactions(1).has_value());

  // insert a different entry {3}
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({0}), make_rcq_entry({2})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();

  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 2);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 2);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({0}));
  ASSERT_EQ(block.value()->entries[1], make_rcq_entry({2}));
}

// Stress_ConsumeFromNonExistentBlock
//
// Insert entries in block 1, then in a new block consume with
// max_fully_consumed_block_number pointing to a block that doesn't exist
// (e.g., block 5). The try_atomic_change_lock code at line 200 reads
// intermediate blocks with CHECK(block.has_value()), which would crash
// if the block doesn't exist in the DAO.
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressConsumeFromNonExistentBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_bad_consume");

  // Insert entries in block 1.
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();

  // Now try to consume blocks up to block 5 (which doesn't exist).
  // max_fully_consumed_block_number=5 means blocks 2..5 need to be read.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(5, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  // This is invalid input from the executor
  ASSERT_FALSE(r.has_value());
}

// StressBackwardFullBlockConsume
//
// Targets lines 204-207 of retrocausal_queue_lock.cc:
//   if (new_consume.max_fully_consumed_block_number <
//       snapshot_consume.max_fully_consumed_block_number)
//
// Sets up a state where the snapshot already has max_fully_consumed > 0,
// then crafts a consume that attempts to move the pointer backward.
// This exercises the "unconsuming" guard: once a block is marked fully
// consumed, a new transaction in the same block cannot regress that state.
TEST_F(DirectRCQTests, StressBackwardFullBlockConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_backward_consume");

  // Block 1: Insert entry.
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert another entry so we have blocks 1 and 2.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta({make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Consume block 1 fully. After this block, the snapshot's
  // max_fully_consumed_block_number will be 1.
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
  advance_block();

  // Block 4: Attempt to consume with max_fully_consumed=0 (backward).
  // The snapshot has max_fully_consumed=1, so setting it to 0 regresses.
  auto tx4 = make_new_per_tx_db_proxy();
  tx4.add_delta(key, make_rcq_consume_delta(0, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx4.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error().code(), SystemErrorCode::Unrecoverable);
}

TEST_F(DirectRCQTests, StressTypeTransitionDeleteRCQDeleteSameTx)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_type_trans");

  auto tx = make_new_per_tx_db_proxy();

  // Step 1: Write a bytestring.
  tx.add_delta(key, make_bytestring_delta("hello"),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Step 2: Delete it.
  tx.add_delta(key, make_delete_delta(),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Step 3: Create an RCQ with some entries.
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({10}), make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Step 4: Consume entries but drop Step 3's inserts from the cumulative
  // delta. This is invalid executor behavior (violates prefix property).
  // ExclusiveLock now validates RCQ deltas via RetrocausalQueueLock delegation,
  // so the invalid input is caught at lock time (consumed > live + added).
  tx.add_delta(key,
               make_rcq_consume_insert_delta(
                   0, 0, {make_rcq_entry({10}), make_rcq_entry({20})}, {}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// Validation-mode stress tests
// ---------------------------------------------------------------------------

// StressValidationModeConsumeNonexistentCurrentBlockEntry
//
// In the same block: TX1 inserts {A}, TX2 consumes {B} from current block
// (where B was never inserted). At block finalization, write_committed_change
// builds a consumed_set from current_block_consumed and tries to match against
// added_entries_. Since {B} was never inserted, consumed_set won't be emptied.
TEST_F(DirectRCQTests, StressValidationConsumeNonexistentCurrentEntry)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  for (auto mode : {BlockExecutionMode::kBlockExecutionModeValidation,
                    BlockExecutionMode::kBlockExecutionModeProposal}) {
    advance_block(mode);

    auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_ghost");

    // TX1 inserts entry {10}
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
                  std::to_underlying(StorageDeltaFlags::None));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // TX2 consumes {99} from current block -- an entry that was never inserted
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(key, make_rcq_consume_delta(0, 0, {make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
    r = tx2.try_push_current_subtransaction();

    // Consumed entry {99} doesn't match any insert, but lock-time validation
    // doesn't check current-block consumed entries. Push succeeds in both
    // modes.
    ASSERT_TRUE(r.has_value() && r.value());

    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());

    // prepare_committed_changes detects the consumed_set mismatch and
    // returns kInvalidExecutorBehavior. reject_block verifies no DAO mutation.
    reject_block(mode);
  }
}

// StressConsumeMoreCurrentThanInserted
//
// TX1 inserts one entry, TX2 consume_inserts specifying two current-block
// consumed entries (one real, one ghost). At finalization the consumed_set
// check should fail.
TEST_F(DirectRCQTests, StressConsumeMoreCurrentThanInserted)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_extra");

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // TX2 consumes two entries from current block but only one was inserted
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0,
                                       {make_rcq_entry({10}),
                                        make_rcq_entry({20})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  // Lock-time validation doesn't check current-block consumed entries.
  // Push succeeds; mismatch caught at prepare_committed_changes.
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  // consumed_set check at prepare detects the extra consumed entry.
  reject_block(BlockExecutionMode::kBlockExecutionModeProposal);
}

// StressConsumeMoreCurrentThanInserted
//
// TX1 inserts one entry, TX2 consume_inserts specifying two current-block
// consumed entries (one real, one ghost). At finalization the consumed_set
// check should fail.
TEST_F(DirectRCQTests, StressValidationConsumeMoreCurrentThanInserted)
{
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_extra");

  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // TX2 consumes two entries from current block but only one was inserted
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0,
                                       {make_rcq_entry({10}),
                                        make_rcq_entry({20})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()); // accepted in validation mode

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  // consumed_set check at prepare detects the extra consumed entry.
  reject_block(BlockExecutionMode::kBlockExecutionModeValidation);
}

// StressSameTxConsumeInsertMismatch
//
// Single TX does a consume_insert where consumed current-block entries don't
// match the inserted entries. This is the same-TX variant:
// insert {10, 20}, then push a consume_insert that consumes {10, 30} (30
// was never inserted). At finalization CHECK(consumed_set.empty()) fires.
TEST_F(DirectRCQTests, StressSameTxConsumeInsertMismatch)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_mismatch");

  auto tx = make_new_per_tx_db_proxy();

  // Sub1: insert two entries
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({10}), make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: consume_insert consuming {10, 30} (30 never inserted) and inserting
  // {40}
  tx.add_delta(key,
               make_rcq_consume_insert_delta(
                   0, 0, {make_rcq_entry({10}), make_rcq_entry({30})},
                   {make_rcq_entry({10}), make_rcq_entry({20}),
                    make_rcq_entry({40})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  // Lock-time validation doesn't check current-block consumed entries.
  // Push succeeds; mismatch caught at prepare_committed_changes.
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  // consumed_set check at prepare detects the mismatched entry.
  reject_block(BlockExecutionMode::kBlockExecutionModeProposal);
}

TEST_F(DirectRCQTests, StressValidationSameTxConsumeInsertMismatch)
{
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_mismatch");

  auto tx = make_new_per_tx_db_proxy();

  // Sub1: insert two entries
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({10}), make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: consume_insert consuming {10, 30} (30 never inserted) and inserting
  // {40}
  tx.add_delta(key,
               make_rcq_consume_insert_delta(
                   0, 0, {make_rcq_entry({10}), make_rcq_entry({30})},
                   {make_rcq_entry({10}), make_rcq_entry({20}),
                    make_rcq_entry({40})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  // this is valid in validation mode
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  // consumed_set check at prepare detects the mismatched entry.
  reject_block(BlockExecutionMode::kBlockExecutionModeValidation);
}

// StressValidationConsumeCurrentBlockZeroInserted
//
// No entries are inserted in the current block, but a consume delta tries to
// consume entries from the current block. This should cause
// CHECK(consumed_set.empty()) to fire at finalization since there are no
// added_entries_ to match against.
TEST_F(DirectRCQTests, StressValidationConsumeCurrentBlockZeroInserted)
{
  for (auto mode : {BlockExecutionMode::kBlockExecutionModeProposal,
                    BlockExecutionMode::kBlockExecutionModeValidation}) {
    advance_block(mode);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_noins");

    // Insert entries in block 1
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                  std::to_underlying(StorageDeltaFlags::None));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
    advance_block(BlockExecutionMode::kBlockExecutionModeValidation);

    // TX in new block: consume from current block even though nothing was
    // inserted in this block. The consume_delta says current_block_consumed={1}
    // but added_entries_ is empty at the lock level.
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(key, make_rcq_consume_delta(0, 0, {make_rcq_entry({1})}),
                  std::to_underlying(StorageDeltaFlags::None));
    r = tx2.try_push_current_subtransaction();
    // Lock-time validation doesn't check current-block consumed entries.
    // Push succeeds; mismatch caught at prepare_committed_changes.
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());
    // consumed_set check at prepare detects that consumed entry {1}
    // is not among the added_entries_ for this block.
    reject_block(mode);
  }
}

// StressValidationConcurrentInsertAndConsumeCurrentBlock
//
// In validation mode, TX1 inserts entries, TX2 (separate TX) tries to consume
// those entries from the current block. At finalization, the lock merges all
// added_entries_ and then tries to match current_block_consumed against them.
// But if TX1 and TX2 commit in a specific order, the accounting might break.
TEST_F(DirectRCQTests, StressValidationConcurrentInsertAndConsumeCurrentBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_conc");

  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // TX1: insert entries
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key,
                make_rcq_insert_delta(
                    {make_rcq_entry({10}), make_rcq_entry({20})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // TX2: consume one of those entries from current block
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(0, 0, {make_rcq_entry({10})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  // Commit consume FIRST, then insert -- stresses the ordering.
  // This is valid: TX2 consumes {10} from current-block added_entries.
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();
}

// StressValidationConsumeMaxBlockEqualLiveBlock
//
// Insert entries in block 1, advance. Then consume with
// max_fully_consumed_block_number equal to max_live_block_num (=1).
// This fully consumes the only block, but then the code needs to read
// block 1 at line 192 (first_block_num = old_consume.max_fully_consumed + 1
// = 0 + 1 = 1). Since we're consuming up to block 1, this should work.
// But what if we ALSO claim partial_consumed > 0? That would mean
// we say block 1 is fully consumed AND we've partially consumed some more
// entries from some non-existent block.
TEST_F(DirectRCQTests, StressValidationConsumeMaxBlockWithPartial)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_maxblk");

  // Insert entries and persist in block 1
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key,
                make_rcq_insert_delta(
                    {make_rcq_entry({1}), make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Consume block 1 fully AND claim partial_consumed=5 from the next block.
  // But there is no next block. This exercises whether partial_consumed
  // on a non-existent next block causes issues in the metadata update at
  // write_committed_change.
  // This constitutes invalid input from the executor
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(1, 5, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressValidationPartialConsumeDecrement
//
// What happens if partial_consumed goes BACKWARDS? i.e., we consume 3 entries
// from block 1, then in a later block we say partial_consumed=1 (fewer than
// what was already consumed). The code at line 185 computes
// (new_partial - old_partial) which would be (1 - 3) = underflow for uint32_t.
TEST_F(DirectRCQTests, StressValidationPartialConsumeDecrement)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_val_partdec");

  // Insert 4 entries
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key,
                make_rcq_insert_delta({make_rcq_entry({1}), make_rcq_entry({2}),
                                       make_rcq_entry({3}),
                                       make_rcq_entry({4})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Consume 3 entries
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(0, 3, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Now try to go backwards: partial_consumed=1 (less than the already-consumed
  // 3) At line 185: consumed_entries += (1 - 3) which is uint32_t underflow
  auto tx3 = make_new_per_tx_db_proxy();
  // This constitutes invalid input from the executor
  tx3.add_delta(key, make_rcq_consume_delta(0, 1, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressConsumeInsertNoOpConsumeFollowedByInsertOnly
//
// In a single TX: subtransaction 1 does a consume_insert with a no-op consume
// (max_fully_consumed=0, partial=0, current_block_consumed={}), then
// subtransaction 2 does an insert-only delta. An honest executor's cumulative
// delta never drops a consume, so narrow() returns kInvalidExecutorBehavior.
// This test verifies the error is detected and propagated.
TEST_F(DirectRCQTests, StressConsumeInsertNoOpConsumeFollowedByInsertOnly)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_noop_consume");

  auto tx = make_new_per_tx_db_proxy();

  // Sub1: consume_insert with no-op consume and one insert
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: insert-only delta (no consume). An honest executor never drops a
  // consume from its cumulative delta, so narrow() returns
  // kInvalidExecutorBehavior.
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({10}), make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value());
}

// StressConsumeForwardThenBackwardInSubtransactions
//
// In a single TX: sub1 consumes partial=2, sub2 consumes partial=1
// (going backward). The narrow() check at item_change.cc line 109-115 checks
// monotonicity of the consume. With past_block_consume having (max_fully=0,
// partial=2) vs (max_fully=0, partial=1): (0,2) < (0,1) is false, and
// (0,2) == (0,1) is false, so the CHECK fires.
TEST_F(DirectRCQTests, StressConsumeForwardThenBackwardInSubtransactions)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_consume_bwd");

  // Setup: insert entries in past block
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta({make_rcq_entry({1}),
                                            make_rcq_entry({2}),
                                            make_rcq_entry({3})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Sub1: consume partial=2
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(0, 2, {}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: consume partial=1 (backward!) -- violates monotonicity in narrow().
  // narrow() returns kInvalidExecutorBehavior because the cumulative delta
  // property requires consumes to be monotonically non-decreasing within a
  // transaction. An honest executor never produces this sequence.
  tx.add_delta(key, make_rcq_consume_delta(0, 1, {}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value());
}

// StressPopSubtransactionWithConsumeDelta
//
// Similar to Stress_PopSubtransactionWithRCQInsert, but tests rollback of a
// CONSUME delta rather than an insert delta. Sub1 does consume_insert with
// partial=1, sub2 does consume_insert with partial=2 (monotonically
// increasing). Pop sub2. The rollback calls modify_lock(after=sub2_consume,
// before=sub1_consume). The question is whether the consumed_ atomic and
// consumed_past_entries_ are handled correctly during rollback.
TEST_F(DirectRCQTests, StressPopSubtransactionWithConsumeDelta)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_pop_consume");

  // Setup: insert entries in past block
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta({make_rcq_entry({1}),
                                            make_rcq_entry({2}),
                                            make_rcq_entry({3})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Sub1: consume_insert with partial=1 and insert {10}
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 1, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: consume_insert with partial=2 (more consume) and more inserts
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 2, {},
                                             {make_rcq_entry({10}),
                                              make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Pop sub2 -- rollback the deeper consume to the shallower one
  auto pop_r = tx.pop_subtransactions(1);
  ASSERT_TRUE(pop_r.has_value()) << "pop_subtransactions returned error";

  // Commit the remaining sub1
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressConsumeInsertDuplicateCurrentConsumedMatchesInserted
TEST_F(DirectRCQTests,
       StressConsumeInsertDuplicateCurrentConsumedExceedsInserted)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);

  for (auto mode : {BlockExecutionMode::kBlockExecutionModeProposal,
                    BlockExecutionMode::kBlockExecutionModeValidation}) {
    advance_block(mode);
    auto key =
        make_contract_data_addrkey(kContractAddress, "rcq_dup_consume_exceed");

    // TX1: insert one copy of {10}
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
                  std::to_underlying(StorageDeltaFlags::None));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // TX2: consume_insert consuming {10, 10} from current block (2 copies)
    // but only 1 copy of {10} exists in added_entries_
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(key,
                  make_rcq_consume_insert_delta(0, 0,
                                                {make_rcq_entry({10}),
                                                 make_rcq_entry({10})},
                                                {make_rcq_entry({20})}),
                  std::to_underlying(StorageDeltaFlags::None));
    r = tx2.try_push_current_subtransaction();

    // Lock-time validation doesn't check current-block consumed entries.
    // Push succeeds in both modes; mismatch caught at prepare.
    ASSERT_TRUE(r.has_value() && r.value());

    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());
    // consumed_set check at prepare catches the duplicate consumption.
    reject_block(mode);
  }
}

// StressDeleteExistingRCQThenRecreateThenConsumeInSameBlock
//
// Block 1: Create RCQ with entries, advance.
// Block 2: TX1 deletes the key (DeletionLock path). TX2 creates new RCQ inserts
// and also consumes from current block.
// The delete goes through DeletionLock::write_committed_change which calls
// RetrocausalQueueLock::cleanup_on_deletion. Then the new RCQ goes through
// RetrocausalQueueLock::write_committed_change. Both operate on the same key's
// merkle trie entries and DAO records. Since they're different ItemLock
// instances (one DeletionLock, one RetrocausalQueueLock), there may be a
// conflict in the trie operations.
TEST_F(DirectRCQTests,
       StressDeleteExistingRCQThenRecreateThenConsumeInSameBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_del_recreate_consume");

  // Block 1: Create RCQ with entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Same TX: delete, then recreate with inserts, then consume from
  // current block
  auto tx = make_new_per_tx_db_proxy();

  // Sub1: delete the key
  tx.add_delta(key, make_delete_delta(),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: recreate as RCQ and insert entries
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({10}), make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  // Sub3: consume from current block (entry {10} is in this sub's inserts)
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({10})},
                                             {make_rcq_entry({10}),
                                              make_rcq_entry({20}),
                                              make_rcq_entry({30})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  // The delete→recreate→consume_insert sequence promotes to exclusive mode.
  // Sub3's inserts {10, 20, 30} include the consumed entry {10}, so the
  // consumed_set check at finalization passes.
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressConsumeFromSameBlockPartialExceedsBlockSize
TEST_F(DirectRCQTests, StressSameBlockPartialOverconsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_same_block_partial_over");

  // Block 1: Insert 2 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Consume with same max_fully_consumed=0, partial=5
  // (same-block branch, overconsume)
  // This constitutes invalid input from the executor
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(0, 5, {}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  // Lock-time validation now catches partial overconsume (partial=5 > block
  // size=2) and returns kInvalidExecutorBehavior.
  ASSERT_FALSE(r.has_value());
}

// StressNNIntDeleteRCQConsumeAllSameTx
//
// Single TX: NNInt set_add on key, push. Delete key, push. RCQ insert {a,b},
// push. RCQ consume_insert consuming {a,b} from current block (live->0), push.
// NNInt → delete → RCQ promotes to exclusive. ExclusiveLock delegates to
// RetrocausalQueueLock for validation, then computes tight cost (0 for empty
// queue).
TEST_F(DirectRCQTests, StressNNIntDeleteRCQConsumeAllSameTx)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_nnint_trans");

  for (auto mode : {BlockExecutionMode::kBlockExecutionModeProposal,
                    BlockExecutionMode::kBlockExecutionModeValidation}) {
    advance_block();
    // First establish the key as NNInt in the snapshot
    auto tx_setup = make_new_per_tx_db_proxy();
    tx_setup.add_delta(key, make_nnint_set_add_delta(0, 100),
                       std::to_underlying(StorageDeltaFlags::None));
    auto r = tx_setup.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
    advance_block(mode);

    // Now in a single TX: NNInt -> delete -> RCQ insert -> RCQ consume all
    auto tx = make_new_per_tx_db_proxy();

    // Sub1: Delete the NNInt key
    tx.add_delta(key, make_delete_delta(),
                 std::to_underlying(StorageDeltaFlags::None));
    r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // Sub2: RCQ insert two entries
    tx.add_delta(key,
                 make_rcq_insert_delta(
                     {make_rcq_entry({10}), make_rcq_entry({20})}),
                 std::to_underlying(StorageDeltaFlags::None));
    r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // Sub3: consume_insert consuming both entries. Cumulative delta preserves
    // Sub2's inserts (prefix property) and adds a consume.
    tx.add_delta(key,
                 make_rcq_consume_insert_delta(
                     0, 0, {make_rcq_entry({10}), make_rcq_entry({20})},
                     {make_rcq_entry({10}), make_rcq_entry({20})}),
                 std::to_underlying(StorageDeltaFlags::None));
    r = tx.try_push_current_subtransaction();
    // NNInt -> delete -> RCQ promotes to exclusive. ExclusiveLock sees
    // 2 inserts, 2 consumed on fresh key -> total = 0 -> cost 0.
    ASSERT_TRUE(r.has_value() && r.value());

    ASSERT_TRUE(tx.commit_transaction({}).has_value());
    advance_block(mode);
  }
}

// StressExclusiveFlagConsumeOnlyPartialOnExistingRCQ
//
// Target: invoke_tx.cc:195 CHECK_LE(cost_change, locked_storage_cost)
// through a NOVEL trigger: exclusive-flag consume-only (no inserts) that
// partially consumes an existing RCQ.
//
// ExclusiveLock::try_atomic_change_lock returns 0 for RCQ with 0 inserts.
// snapshot_cost_transform(0, cost(N), permanent_exclusive=true) = 0 - cost(N) =
// -cost(N). locked_storage_cost = -cost(N). At commit: exclusive downgrade
// creates fresh RetrocausalQueueLock. commit_change: live_entries = N-1
// (consumed 1 of N). cost_change = cost(N-1) - cost(N). This is negative but >
// -cost(N) since cost(N-1) > 0. CHECK(cost(N-1) - cost(N) <= -cost(N)) =>
// CHECK(cost(N-1) <= 0) => FAILS for N >= 2.
TEST_F(DirectRCQTests, StressExclusiveFlagConsumeOnlyPartialOnExistingRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_excl_consume_only_partial");

  // Block 1: Insert 3 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta({make_rcq_entry({1}),
                                            make_rcq_entry({2}),
                                            make_rcq_entry({3})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Consume-only (no inserts) with EXCLUSIVE flag.
  // Consume partial: max_fully_consumed=0, partial=1 (consume 1 of 3 entries).
  // ExclusiveLock returns 0 for 0 inserts.
  // snapshot_cost_transform(0, cost(3), true) = -cost(3).
  // At commit: cost_change = cost(2) - cost(3) > -cost(3) => CHECK fails.
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(0, 1, {}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressExclusiveFlagConsumeInsertFewerThanSnapshotOnExistingRCQ
//
// Target: invoke_tx.cc:195 CHECK_LE(cost_change, locked_storage_cost)
// Similar to above but with consume_insert where inserts < snapshot entries.
//
// ExclusiveLock returns cost(inserts) for RCQ.
// snapshot_cost_transform(cost(inserts), cost(N), true) = cost(inserts) -
// cost(N). If inserts < N, locked_storage_cost is NEGATIVE. At commit: fresh
// lock processes consume + insert. consumed_past_entries_ from cross-block
// consume, plus new inserts. cost_change = cost(N + inserts - consumed) -
// cost(N). If consumed < inserts, cost_change could be positive or less
// negative than locked. Key: consumed=0 (same-block partial=0), inserts=1, N=5.
// cost_change = cost(5+1) - cost(5) = cost(6) - cost(5) > 0 > -cost(5)+cost(1).
// CHECK(cost(6)-cost(5) <= cost(1)-cost(5)) => CHECK(cost(6) <= cost(1)) =>
// FAILS!
TEST_F(DirectRCQTests,
       StressExclusiveFlagConsumeInsertFewerThanSnapshotOnExistingRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_excl_ci_fewer");

  // Block 1: Insert 5 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2}),
                          make_rcq_entry({3}), make_rcq_entry({4}),
                          make_rcq_entry({5})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: consume_insert with exclusive flag.
  // Consume: no-op (max_fully_consumed=0, partial=0).
  // Insert: 1 entry. This is fewer than the 5 snapshot entries.
  // ExclusiveLock returns cost(1). locked_storage_cost = cost(1) - cost(5) < 0.
  // At commit: fresh lock. live_entries = 5+1 = 6. cost_change =
  // cost(6)-cost(5) > 0. CHECK(cost(6)-cost(5) <= cost(1)-cost(5)) => cost(6)
  // <= cost(1) => FAILS!
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressExclusiveFlagInsertOnlyFewerThanSnapshotOnExistingRCQ
//
// Target: invoke_tx.cc:195 CHECK_LE(cost_change, locked_storage_cost)
// Simplest case: exclusive RCQ insert-only with fewer entries than snapshot.
//
// ExclusiveLock returns cost(1). locked_storage_cost = cost(1) - cost(5) < 0.
// At commit: fresh lock. No pessimistic reserve since before=nullptr means
// delta is processed fresh. Actually: the downgrade calls
// try_atomic_change_lock(nullptr, inner, ctx). inner has 1 insert.
// meta.live_entries = 5. new_live_entries = 5+1 = 6.
// absolute_cost = cost(6). Then pessimistic reserve:
// !empty && meta.live_entries > 0 => max(cost(6), cost(5)+cost(1)) =
// cost(5)+cost(1). CHECK(r.has_value()) passes.
//
// commit_change: cost_before = cost(5) (snapshot, no prior deltas).
// elements_added = 1. No consume. element_delta = 1.
// committed_element_count_delta_ = 1.
// live_entries = 5+1 = 6. cost_after = cost(6).
// cost_change = cost(6) - cost(5) > 0.
// CHECK(cost(6)-cost(5) <= cost(1)-cost(5)) => cost(6) <= cost(1) => FAILS!
TEST_F(DirectRCQTests,
       StressExclusiveFlagInsertOnlyFewerThanSnapshotOnExistingRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_insert_fewer");

  // Block 1: Insert 5 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2}),
                          make_rcq_entry({3}), make_rcq_entry({4}),
                          make_rcq_entry({5})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert-only 1 entry with exclusive flag on existing RCQ key.
  // ExclusiveLock returns cost(1). locked_storage_cost = cost(1) - cost(5) < 0.
  // At commit: cost_change = cost(6) - cost(5) > 0 > locked_storage_cost.
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressExclusiveFlagInsertOverMaxEntriesPerBlock
//
// Target: item_lock.cc:130 CHECK(r.has_value())
//
// ExclusiveLock::try_atomic_change_lock does NOT enforce
// kRetrocausalQueueBlockEntriesMaxLen (1000). It just returns cost(entries).
// But at commit time, the exclusive downgrade creates a fresh
// RetrocausalQueueLock and calls try_atomic_change_lock(nullptr, inner, ctx).
// The fresh lock's reserved_entry_count_ is 0. The insert has 1001 entries.
// reserved_entry_count_.fetch_add(1001) = 1001 > 1000 -> returns kConflict.
// CHECK(r.has_value()) at item_lock.cc:130 fires.
TEST_F(DirectRCQTests, StressExclusiveFlagInsertOverMaxEntriesPerBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_excl_over_max");

  // Block 1: Create a small RCQ
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert 1001 entries with exclusive flag.
  // ExclusiveLock doesn't check entry count. Push succeeds.
  // At commit: downgrade creates RetrocausalQueueLock with
  // reserved_entry_count_=0. try_atomic_change_lock: 0 + 1001 > 1000 ->
  // kConflict -> CHECK fails.
  // This consitutes invalid input from the executor
  std::vector<QueueEntry> entries;
  entries.reserve(1001);
  for (int i = 0; i < 1001; ++i) {
    entries.push_back(make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                                      static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_insert_delta(entries),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  // ExclusiveLock rejects > kRetrocausalQueueBlockEntriesMaxLen with kConflict.
  // kConflict maps to has_value=true, value=false.
  ASSERT_TRUE(r.has_value() && !r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressExclusiveConsumeInsertOnNewKeyCurrentBlockMismatch
// ExclusiveLock validates consumed entry identity at lock time:
// consumed {99} not in added_entries {10} → rejected.
TEST_F(DirectRCQTests, StressExclusiveConsumeInsertOnNewKeyCurrentBlockMismatch)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_new_mismatch");
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({99})},
                                             {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  // ExclusiveLock validates consumed entry identity at lock time.
  // {99} not in added_entries {10} → rejected.
  ASSERT_FALSE(r.has_value());
}

// StressExclusiveConsumeInsertOnExistingRCQCurrentBlockMismatch
// ExclusiveLock validates consumed entry identity on existing RCQ:
// consumed {77} not in added_entries {10} → rejected.
TEST_F(DirectRCQTests,
       StressExclusiveConsumeInsertOnExistingRCQCurrentBlockMismatch)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_exist_mismatch");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta({make_rcq_entry({1}),
                                            make_rcq_entry({2}),
                                            make_rcq_entry({3})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({77})},
                                             {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  // ExclusiveLock validates consumed entry identity at lock time.
  // {77} not in added_entries {10} → rejected.
  ASSERT_FALSE(r.has_value());
}

TEST_F(DirectRCQTests,
       StressExclusiveConsumeInsertOnExistingRCQCurrentBlockMismatchValidation)
{
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_exist_mismatch");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta({make_rcq_entry({1}),
                                            make_rcq_entry({2}),
                                            make_rcq_entry({3})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({77})},
                                             {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  // ExclusiveLock validates consumed entry identity at lock time.
  // {77} not in added_entries {10} → rejected.
  ASSERT_FALSE(r.has_value());
}

// StressExclusiveConsumeInsertOverconsumeCurrentBlock
// ExclusiveLock detects overconsume (5 consumed from block with 2 entries)
// and returns kInvalidExecutorBehavior.
TEST_F(DirectRCQTests, StressExclusiveConsumeInsertOverconsumeCurrentBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_overconsume_cur");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(
      key,
      make_rcq_consume_insert_delta(0, 0,
                                    {make_rcq_entry({10}), make_rcq_entry({20}),
                                     make_rcq_entry({30}), make_rcq_entry({40}),
                                     make_rcq_entry({50})},
                                    {}),
      std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

TEST_F(DirectRCQTests,
       StressExclusiveConsumeInsertOverconsumeCurrentBlockValidationMode)
{
  advance_block(BlockExecutionMode::kBlockExecutionModeValidation);
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_overconsume_cur");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(
      key,
      make_rcq_consume_insert_delta(0, 0,
                                    {make_rcq_entry({10}), make_rcq_entry({20}),
                                     make_rcq_entry({30}), make_rcq_entry({40}),
                                     make_rcq_entry({50})},
                                    {}),
      std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  // ExclusiveLock detects overconsume (total < 0) and returns
  // kInvalidExecutorBehavior.
  ASSERT_FALSE(r.has_value());
}

// StressExclusiveConsumeInsertPastBlockOnFreshKey
// ExclusiveLock rejects consume of past block on fresh key (no metadata).
TEST_F(DirectRCQTests, StressExclusiveConsumeInsertPastBlockOnFreshKey)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_past_fresh");
  auto tx = make_new_per_tx_db_proxy();
  // This consitutes invalid input from the executor
  tx.add_delta(key,
               make_rcq_consume_insert_delta(1, 0, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

TEST_F(DirectRCQTests,
       StressExclusiveConsumeInsertPastBlockOnExistingRCQValidCase)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_past_fresh");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  auto tx = make_new_per_tx_db_proxy();
  // This consitutes invalid input from the executor
  tx.add_delta(key,
               make_rcq_consume_insert_delta(1, 0, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressPopSubtransactionExclusiveToNonExclusiveRCQ
TEST_F(DirectRCQTests, StressPopSubtransactionExclusiveToNonExclusiveRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_pop_excl_to_nonexcl");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2}),
                          make_rcq_entry({3}), make_rcq_entry({4}),
                          make_rcq_entry({5})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 5, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx.add_delta(key, make_bytestring_delta("hello, world!"),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.pop_subtransactions(1).has_value());
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressExclusiveRCQConsumeOnlyOnFreshKey
// ExclusiveLock rejects consume-only on fresh key (total entries < 0).
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusiveRCQConsumeOnlyOnFreshKey)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_consume_fresh");
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(0, 3, {}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressExclusiveConsumeInsertPastBlockOnExistingRCQ
// ExclusiveLock rejects consume of block beyond max_live on existing RCQ.
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusiveConsumeInsertPastBlockOnExistingRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_excl_consume_past_exist");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(3, 0, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressMultiKeyExclusiveAndNormalRCQSameTx
// Verifies a TX can mix exclusive and normal RCQ ops on different keys.
TEST_F(DirectRCQTests, StressMultiKeyExclusiveAndNormalRCQSameTx)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto keyA = make_contract_data_addrkey(kContractAddress, "rcq_multi_A");
  auto keyB = make_contract_data_addrkey(kContractAddress, "rcq_multi_B");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(keyB,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2}),
                          make_rcq_entry({3}), make_rcq_entry({4}),
                          make_rcq_entry({5})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(keyA, make_rcq_insert_delta({make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::None));
  tx.add_delta(keyB, make_rcq_insert_delta({make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressConsumeAfterFullConsumeAndReinsertSameBlock
// Rejects overconsume (partial=5) on block with only 3 entries.
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressConsumeAfterFullConsumeAndReinsertSameBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_consume_reinsert");
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key,
                make_rcq_insert_delta(
                    {make_rcq_entry({1}), make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {},
                                              {make_rcq_entry({10}),
                                               make_rcq_entry({20}),
                                               make_rcq_entry({30})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block();
  auto tx3 = make_new_per_tx_db_proxy();
  // This is invalid input from the executor.
  tx3.add_delta(key, make_rcq_consume_delta(2, 5, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressExclusiveRCQWithVeryLargePartialConsume
// ExclusiveLock rejects partial=UINT32_MAX on block with 2 entries.
//
// This constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusiveRCQWithVeryLargePartialConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_huge_partial");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(0, UINT32_MAX, {}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressExclusiveConsumeBlockBeyondMaxLiveOnExistingRCQ
// ExclusiveLock rejects consume of block 5 when max_live is 1.
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusiveConsumeBlockBeyondMaxLiveOnExistingRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_excl_consume_beyond_max");
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(5, 0, {}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();

  // invalid input guard
  ASSERT_FALSE(r.has_value());
}

// Rejects partial overconsume (partial=3 on block with 2 entries) at lock
// time.
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusiveConsumeOnlyPartialGreaterThanBlockSize)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_partial_gt_block_size");

  // Block 1: Insert 2 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert 5 + consume partial=3 on block with 2 entries.
  // consumed_entries = 3. element_delta = 5 - 3 = 2. live_entries = 2+2=4. OK.
  // Metadata persisted: consume_state={max_fully_consumed=0, partial=3},
  // max_live_block_num=2, live_entries=4.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(
      key,
      make_rcq_consume_insert_delta(0, 3, {},
                                    {make_rcq_entry({10}), make_rcq_entry({20}),
                                     make_rcq_entry({30}), make_rcq_entry({40}),
                                     make_rcq_entry({50})}),
      std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// ExclusiveLock rejects partial overconsume (partial=5 on block with 2
// entries) at lock time before any metadata corruption can occur.
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusivePartialOverconsumeFollowedByFullConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_excl_prtl_overcons_full");

  // Block 1: Insert 2 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Exclusive consume_insert with partial=5 (overconsume by 3).
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_insert_delta(
                    0, 5, {},
                    {make_rcq_entry({10}), make_rcq_entry({20}),
                     make_rcq_entry({30}), make_rcq_entry({40}),
                     make_rcq_entry({50}), make_rcq_entry({60}),
                     make_rcq_entry({70}), make_rcq_entry({80})}),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// ExclusiveLock rejects partial overconsume (partial=5 on block with 2
// entries) at lock time, preventing same-block consume corruption.
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests,
       StressExclusivePartialOverconsumeFollowedBySameBlockConsume)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_excl_prtl_overcons_same");

  // Block 1: Insert 2 entries
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Exclusive partial overconsume + enough inserts to keep live >= 0
  // partial=5, inserts=8. element_delta = 8 - 5 = 3. live = 2+3 = 5.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_insert_delta(
                    0, 5, {},
                    {make_rcq_entry({10}), make_rcq_entry({20}),
                     make_rcq_entry({30}), make_rcq_entry({40}),
                     make_rcq_entry({50}), make_rcq_entry({60}),
                     make_rcq_entry({70}), make_rcq_entry({80})}),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressExclusiveConsumeOnlyOnFreshKeyNoMetadata
//
// Target: retrocausal_queue_lock.cc get_meta returns fresh metadata
//         for a non-existent key, then exclusive consume operates on it.
//         At commit: fresh RCQ lock loads metadata (fresh).
//         consume_state = {0,0}. new_consume = {1,0}. Cross-block path.
//         first_block_num = 0+1 = 1. Read block 1 -> kNotFound.
//         CHECK(first_block.has_value()) at line 193 fires.
//         This hits site #2 but from a fresh key (no prior existence).
//
// This test constitutes invalid inputs from the executor.
TEST_F(DirectRCQTests, StressExclusiveConsumeOnlyOnFreshKeyNoMetadata)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_fresh_excl_consume");

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key, make_rcq_consume_delta(1, 0, {}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressExclusiveRCQThenNormalModifySameBlockDowngrade
TEST_F(DirectRCQTests, StressExclusiveRCQThenNormalModifySameBlockDowngrade)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_then_normal");

  // Block 1: Insert 2 entries normally
  auto tx_setup = make_new_per_tx_db_proxy();
  tx_setup.add_delta(key,
                     make_rcq_insert_delta(
                         {make_rcq_entry({1}), make_rcq_entry({2})}),
                     std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_setup.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_setup.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Sub 1 exclusive insert [A]. Sub 2 (same TX) non-exclusive
  // insert [A, B] on same key. After narrow, change becomes exclusive
  // (from earlier exclusive in sub 1). At commit, downgrade.
  auto tx = make_new_per_tx_db_proxy();

  // Sub 1: exclusive insert
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub 2: non-exclusive insert with more entries
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({10}), make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();
}

// StressExclusiveConsumeInsertMismatchedCurrentBlock
TEST_F(DirectRCQTests, StressExclusiveConsumeInsertMismatchedCurrentBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_mismatch_consumed");

  // Fresh key: exclusive consume_insert with mismatched
  // current_block_consumed. Insert [A={10}, B={20}] but consume [{99}] from
  // current block.
  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 0, {make_rcq_entry({99})},
                                             {make_rcq_entry({10}),
                                              make_rcq_entry({20})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  auto r = tx.try_push_current_subtransaction();
  // ExclusiveLock validates consumed entry identity at lock time.
  // {99} not in added_entries {10, 20} → rejected.
  ASSERT_FALSE(r.has_value());
}

TEST_F(DirectRCQTests, StressExclusiveConsumeOnlyZeroBlockFreshKey)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_excl_zero_block");

  auto tx = make_new_per_tx_db_proxy();
  tx.add_delta(key,
               make_rcq_consume_insert_delta(0, 1, {}, {make_rcq_entry({10})}),
               std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  // This constitutes invalid input from the executor
  auto r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// StressExclusiveOverconsumePartialThenInsertOnlyNextBlock
TEST_F(DirectRCQTests, StressExclusiveOverconsumePartialThenInsertOnlyNextBlock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_overcons_then_ins");

  // Block 1: Insert 2 entries
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key,
                make_rcq_insert_delta(
                    {make_rcq_entry({1}), make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Exclusive overconsume partial=5 + 8 inserts.
  // Persists corrupted metadata: partial=5 on block with 2 entries.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_insert_delta(
                    0, 5, {},
                    {make_rcq_entry({10}), make_rcq_entry({20}),
                     make_rcq_entry({30}), make_rcq_entry({40}),
                     make_rcq_entry({50}), make_rcq_entry({60}),
                     make_rcq_entry({70}), make_rcq_entry({80})}),
                std::to_underlying(StorageDeltaFlags::ExclusiveLock));
  r = tx2.try_push_current_subtransaction();
  // ExclusiveLock detects partial overconsume (partial=5 > block size=2)
  // and returns kInvalidExecutorBehavior. The corrupted metadata never gets
  // persisted.
  ASSERT_FALSE(r.has_value()); // invalid input guard
}

// ---------------------------------------------------------------------------
// Overwrite-and-recreate tests
// ---------------------------------------------------------------------------

// Build a multi-block RCQ, partially consume, overwrite with a bytestring,
// recreate the queue in a later block, then consume and verify only the
// newly inserted entry is visible.
TEST_F(DirectRCQTests, OverwritePartiallyConsumedRCQThenRecreate)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_ow_recreate");

  // Block 1: Insert 3 entries.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta({make_rcq_entry({1}),
                                         make_rcq_entry({2}),
                                         make_rcq_entry({3})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert 2 more entries (now two blocks of data).
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({4}), make_rcq_entry({5})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Partially consume — fully consume block 1, consume 1 entry from
  // block 2.  Leaves 1 entry ({5}) unconsumed.
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_rcq_consume_delta(1, 1, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Sanity-check partially consumed state.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 1);
  ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number, 1);
  ASSERT_EQ(queue->metadata().live_entries, 1);

  // Block 4: Overwrite the partially-consumed RCQ with a bytestring.
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_bytestring_delta("overwritten"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Block 5: Delete bytestring, then recreate the RCQ with a single entry.
  auto tx_b5 = make_new_per_tx_db_proxy();
  tx_b5.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b5.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b5.commit_transaction({}).has_value());
  advance_block();

  // Verify: only {99} is visible, no ghost entries from the old queue.
  // RCQ block numbers are per-key counters (max_live_block_num + 1 for each
  // new block of entries), so the recreated queue starts at block 1.
  queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 0);
  ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number, 0);

  // Block 1 of the recreated queue should contain only {99}.
  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  // No ghost blocks should survive from the old queue past
  // max_live_block_num.
  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());

  // Block 6: Consume {99} — fully consume the recreated queue.
  auto tx_b6 = make_new_per_tx_db_proxy();
  tx_b6.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b6.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b6.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// Same scenario but the partial consume and bytestring overwrite happen in
// the same transaction (different subtransactions) within a single block.
TEST_F(DirectRCQTests, ConsumeAndOverwriteRCQSameTxThenRecreate)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_consume_ow_same");

  // Block 1: Insert 3 entries.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta({make_rcq_entry({1}),
                                         make_rcq_entry({2}),
                                         make_rcq_entry({3})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert 2 more entries.
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({4}), make_rcq_entry({5})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Single TX — sub1 partially consumes, sub2 overwrites with
  // bytestring.  Tests type transition after a consume within the same block.
  auto tx_b3 = make_new_per_tx_db_proxy();

  tx_b3.add_delta(key, make_rcq_consume_delta(1, 1, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b3.add_delta(key, make_bytestring_delta("overwritten"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Block 4: Delete bytestring, recreate RCQ with {99}.
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b4.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Verify only {99} is visible.  Recreated queue starts at block 1.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 0);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());

  // Block 5: Consume the single entry.
  auto tx_b5 = make_new_per_tx_db_proxy();
  tx_b5.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b5.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// Overwrite a partially-consumed RCQ, then recreate with multiple entries
// across two blocks.  Verify old data is gone and the recreated queue can
// itself be partially consumed.
TEST_F(DirectRCQTests, OverwritePartiallyConsumedRCQRecreateMultiEntry)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_ow_multi");

  // Block 1: Insert {1}, {2}.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({1}), make_rcq_entry({2})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert {3}.
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key, make_rcq_insert_delta({make_rcq_entry({3})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Partially consume (1 entry from block 1).
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_rcq_consume_delta(0, 1, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Block 4: Overwrite with bytestring.
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_bytestring_delta("dead"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Block 5: Delete + recreate RCQ with 3 entries.
  auto tx_b5 = make_new_per_tx_db_proxy();
  tx_b5.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b5.add_delta(key,
                  make_rcq_insert_delta({make_rcq_entry({10}),
                                         make_rcq_entry({20}),
                                         make_rcq_entry({30})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b5.commit_transaction({}).has_value());
  advance_block();

  // Block 6: Add another entry to the recreated queue in a second block.
  auto tx_b6 = make_new_per_tx_db_proxy();
  tx_b6.add_delta(key, make_rcq_insert_delta({make_rcq_entry({40})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b6.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b6.commit_transaction({}).has_value());
  advance_block();

  // Verify: 4 entries across RCQ blocks 1 and 2, nothing from the old queue.
  // Recreated queue starts numbering from block 1.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 4);
  ASSERT_EQ(queue->metadata().max_live_block_num, 2);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 0);

  auto b1 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 1);
  ASSERT_TRUE(b1.has_value());
  ASSERT_TRUE(b1.value().has_value());
  ASSERT_EQ(b1.value()->block_number, 1);
  ASSERT_EQ(b1.value()->entries.size(), 3);
  ASSERT_EQ(b1.value()->entries[0], make_rcq_entry({10}));
  ASSERT_EQ(b1.value()->entries[1], make_rcq_entry({20}));
  ASSERT_EQ(b1.value()->entries[2], make_rcq_entry({30}));

  auto b2 = get_db_proxy().get_next_retrocausal_queue_block(get_block_number(),
                                                            key, 2);
  ASSERT_TRUE(b2.has_value());
  ASSERT_TRUE(b2.value().has_value());
  ASSERT_EQ(b2.value()->block_number, 2);
  ASSERT_EQ(b2.value()->entries.size(), 1);
  ASSERT_EQ(b2.value()->entries[0], make_rcq_entry({40}));

  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      3);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());

  // Block 7: Partially consume the recreated queue — consume RCQ block 1 only.
  auto tx_b7 = make_new_per_tx_db_proxy();
  tx_b7.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b7.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b7.commit_transaction({}).has_value());
  advance_block();

  // Only RCQ block 2 entry {40} should remain.
  queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 1);

  // Block 1 was pruned; query from block 2 to find the remaining entry.
  auto remaining =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(remaining.has_value());
  ASSERT_TRUE(remaining.value().has_value());
  ASSERT_EQ(remaining.value()->block_number, 2);
  ASSERT_EQ(remaining.value()->entries.size(), 1);
  ASSERT_EQ(remaining.value()->entries[0], make_rcq_entry({40}));
}

// Same pattern as OverwritePartiallyConsumedRCQThenRecreate but overwriting
// with NNInt instead of bytestring.  Verifies the cleanup bug is systematic
// across all non-delete lock types.
TEST_F(DirectRCQTests, OverwriteRCQWithNNIntThenRecreate)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_ow_nnint");

  // Block 1: Insert {1}, {2}, {3}.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta({make_rcq_entry({1}),
                                         make_rcq_entry({2}),
                                         make_rcq_entry({3})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert {4}, {5}.
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({4}), make_rcq_entry({5})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Partially consume — fully consume block 1, consume 1 from block 2.
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_rcq_consume_delta(1, 1, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Block 4: Overwrite with NNInt.
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_nnint_set_add_delta(0, 100),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Block 5: Delete NNInt, then recreate RCQ with {99}.
  auto tx_b5 = make_new_per_tx_db_proxy();
  tx_b5.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b5.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b5.commit_transaction({}).has_value());
  advance_block();

  // Verify: only {99} visible, no ghost blocks from old queue.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);
  ASSERT_EQ(queue->metadata().consume_state.max_fully_consumed_block_number, 0);
  ASSERT_EQ(queue->metadata().consume_state.partial_consumed_number, 0);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());

  // Block 6: Consume {99}.
  auto tx_b6 = make_new_per_tx_db_proxy();
  tx_b6.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b6.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b6.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// Overwrite RCQ with bytestring, then delete the bytestring.  The deletion
// sees a bytestring (not RCQ), so it never calls cleanup_on_deletion() for the
// orphaned RCQ blocks — they persist permanently.
TEST_F(DirectRCQTests, OverwriteRCQWithBytestringDeleteDoesNotCleanGhostBlocks)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_ow_bs_del");

  // Block 1: Insert {1}, {2}.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({1}), make_rcq_entry({2})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Overwrite with bytestring "dead".
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key, make_bytestring_delta("dead"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Delete the bytestring (key is now empty).
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Key should be fully gone now.
  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());

  // Block 4: Insert {99} into a fresh RCQ (no delete needed, key is empty).
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Verify: only {99} visible — ghost blocks from block 1 should be gone.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());
}

// Same as above but with two RCQ blocks before overwrite.  The recreated RCQ's
// block 1 overwrites ghost block 1, but ghost block 2 persists because the
// bytestring delete path doesn't know about RCQ blocks.
TEST_F(DirectRCQTests,
       OverwriteMultiBlockRCQWithBytestringDeleteLeavesGhostBlocks)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_ow_bs_del2");

  // Block 1: Insert {1}, {2} (RCQ block 1).
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({1}), make_rcq_entry({2})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert {3} (RCQ block 2).
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key, make_rcq_insert_delta({make_rcq_entry({3})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Overwrite with bytestring (orphans RCQ blocks 1 and 2).
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_bytestring_delta("dead"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Block 4: Delete the bytestring.
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());

  // Block 5: Insert {99} into fresh RCQ (RCQ block 1 — overwrites ghost 1).
  auto tx_b5 = make_new_per_tx_db_proxy();
  tx_b5.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b5.commit_transaction({}).has_value());
  advance_block();

  // Verify: only {99} visible.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  // Ghost block 2 from the old queue should be gone.
  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());
}

// Regression test: Bytestring → (delete) → RCQ with entries in the same TX.
// The old bytestring shares the same DB key slot as the RCQ metadata, so
// write_retrocausal_queue_metadata() implicitly overwrites it.  This works
// because bytestrings have no side data (unlike RCQ blocks).
TEST_F(DirectRCQTests, BytestringToRCQWithEntriesDoesNotCleanOldBytestring)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_bs_to_rcq");

  // Block 1: Write bytestring "hello".
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key, make_bytestring_delta("hello"),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Same TX — sub1: delete, sub2: RCQ insert {10}, {20}.
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b2.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({10}), make_rcq_entry({20})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Verify queue metadata is correct.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 2);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->entries.size(), 2);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({10}));
  ASSERT_EQ(block.value()->entries[1], make_rcq_entry({20}));

  // Consume the queue fully — key should disappear cleanly.
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// Same as above but with NNInt instead of bytestring.  NNInt also shares the
// same DB key slot, so the overwrite is implicit.
TEST_F(DirectRCQTests, NNIntToRCQWithEntriesOverwritesCleanly)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_nnint_to_rcq");

  // Block 1: Write NNInt value 42.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key, make_nnint_set_add_delta(0, 42),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Verify NNInt is stored.
  auto item = lookup_storage_item_from_db(key);
  ASSERT_TRUE(item.has_value());
  ASSERT_EQ(item->type(), StorageItemType::kItemTypeNonnegativeInt);

  // Block 2: Same TX — sub1: delete, sub2: RCQ insert {10}, {20}.
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b2.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({10}), make_rcq_entry({20})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Verify queue metadata — should be RCQ, not NNInt.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 2);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->entries.size(), 2);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({10}));
  ASSERT_EQ(block.value()->entries[1], make_rcq_entry({20}));

  // Consume fully — key should disappear cleanly.
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// Positive/sanity test: verify the explicit DELETE path works correctly for
// RCQ cleanup.  This should pass and serves as a baseline.
TEST_F(DirectRCQTests, ExplicitDeleteRCQWithBlocksCleansUpProperly)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_del_cleanup");

  // Block 1: Insert {1}, {2}, {3}.
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta({make_rcq_entry({1}),
                                         make_rcq_entry({2}),
                                         make_rcq_entry({3})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Insert {4}, {5} (second block).
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({4}), make_rcq_entry({5})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Explicit delete.
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Key should be gone.
  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());

  // Old RCQ blocks 1 and 2 should be cleaned up.
  auto ghost1 =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(ghost1.has_value());
  ASSERT_FALSE(ghost1.value().has_value());

  auto ghost2 =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(ghost2.has_value());
  ASSERT_FALSE(ghost2.value().has_value());

  // Block 4: Recreate with {99}.
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Verify only {99}.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());
}

// Stress test: two full overwrite cycles (RCQ → bytestring → RCQ → bytestring
// → RCQ) to verify ghost blocks accumulate across cycles.
TEST_F(DirectRCQTests, DoubleOverwriteCycleRCQBytestringRCQ)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_double_ow");

  // Block 1: Insert {1}, {2} into RCQ (RCQ block 1).
  auto tx_b1 = make_new_per_tx_db_proxy();
  tx_b1.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({1}), make_rcq_entry({2})}),
                  std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_b1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: Overwrite with bytestring (orphans RCQ block 1).
  auto tx_b2 = make_new_per_tx_db_proxy();
  tx_b2.add_delta(key, make_bytestring_delta("cycle1"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: Delete + insert {10}, {20} into RCQ (RCQ block 1).
  auto tx_b3 = make_new_per_tx_db_proxy();
  tx_b3.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b3.add_delta(key,
                  make_rcq_insert_delta(
                      {make_rcq_entry({10}), make_rcq_entry({20})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b3.commit_transaction({}).has_value());
  advance_block();

  // Block 4: Insert {30} into RCQ (RCQ block 2).
  auto tx_b4 = make_new_per_tx_db_proxy();
  tx_b4.add_delta(key, make_rcq_insert_delta({make_rcq_entry({30})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b4.commit_transaction({}).has_value());
  advance_block();

  // Block 5: Overwrite with bytestring again (orphans RCQ blocks 1+2).
  auto tx_b5 = make_new_per_tx_db_proxy();
  tx_b5.add_delta(key, make_bytestring_delta("cycle2"),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b5.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b5.commit_transaction({}).has_value());
  advance_block();

  // Block 6: Delete + insert {99} into RCQ (RCQ block 1).
  auto tx_b6 = make_new_per_tx_db_proxy();
  tx_b6.add_delta(key, make_delete_delta(),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b6.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  tx_b6.add_delta(key, make_rcq_insert_delta({make_rcq_entry({99})}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b6.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b6.commit_transaction({}).has_value());
  advance_block();

  // Verify: only {99} visible.
  auto queue = lookup_storage_item_from_db(key);
  ASSERT_TRUE(queue.has_value());
  ASSERT_EQ(queue->type(), StorageItemType::kItemTypeRetrocausalQueue);
  ASSERT_EQ(queue->metadata().live_entries, 1);
  ASSERT_EQ(queue->metadata().max_live_block_num, 1);

  auto block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block.value().has_value());
  ASSERT_EQ(block.value()->block_number, 1);
  ASSERT_EQ(block.value()->entries.size(), 1);
  ASSERT_EQ(block.value()->entries[0], make_rcq_entry({99}));

  // Ghost block 2 from second cycle should be gone.
  auto no_more =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(no_more.has_value());
  ASSERT_FALSE(no_more.value().has_value());

  // Consume {99} — queue should disappear cleanly.
  auto tx_b7 = make_new_per_tx_db_proxy();
  tx_b7.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                  std::to_underlying(StorageDeltaFlags::None));
  r = tx_b7.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_b7.commit_transaction({}).has_value());
  advance_block();

  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// FullSubtransactionRollbackReleasesRCQLock
//
// Insert into an RCQ in a subtransaction, then pop that subtransaction
// entirely (no outer frame touched the key). This exercises the
// try_atomic_change_lock(before=delta, after=nullptr) path — the full
// lock release during rollback. After rollback, verify the key doesn't
// appear in committed state.
TEST_F(DirectRCQTests, FullSubtransactionRollbackReleasesRCQLock)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_full_release");

  auto tx = make_new_per_tx_db_proxy();

  // Push a subtransaction that inserts into the RCQ.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0xAA})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Pop it — this is the ONLY subtransaction that touched this key,
  // so modify_lock is called with after=nullptr, fully releasing the lock.
  auto pop_r = tx.pop_subtransactions(1);
  ASSERT_TRUE(pop_r.has_value()) << "pop_subtransactions returned error";

  // Commit the (now empty) transaction.
  ASSERT_TRUE(tx.commit_transaction({}).has_value());
  advance_block();

  // The RCQ should not exist — the insert was fully rolled back.
  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// ConsumeThreeBlocksInOneDelta
//
// Build a 3-block RCQ (one entry per block), then consume all 3 blocks
// in a single delta. This exercises the intermediate block iteration loop
// in try_atomic_change_lock (lines 255-261 of retrocausal_queue_lock.cc)
// which reads each intermediate block from the DAO to count consumed entries.
TEST_F(DirectRCQTests, ConsumeThreeBlocksInOneDelta)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_three_block_consume");

  // Block 1: insert entry {1}.
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());
  advance_block();

  // Block 2: insert entry {2}.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta({make_rcq_entry({2})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
  advance_block();

  // Block 3: insert entry {3}.
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_insert_delta({make_rcq_entry({3})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
  advance_block();

  // Now consume all 3 blocks at once: max_fully_consumed_block_number=3,
  // partial_consumed_number=0. This means:
  //   - First block (block 1): consumed entirely (1 entry)
  //   - Intermediate block (block 2): consumed entirely (1 entry) — this
  //     is the loop at line 255 that was previously uncovered
  //   - The consume stops at block 3 (fully consumed, partial=0)
  auto tx4 = make_new_per_tx_db_proxy();
  tx4.add_delta(key, make_rcq_consume_delta(3, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx4.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx4.commit_transaction({}).has_value());
  advance_block();

  // The RCQ should be empty (all 3 entries consumed, 0 live).
  ASSERT_FALSE(lookup_storage_item_from_db(key).has_value());
}

// H005 (#1489): An overfull consume+insert transaction must not poison the
// single-consumer bit. A later legitimate consume on the same key in the same
// block must still succeed.
TEST_F(DirectRCQTests, OverfullConsumeInsertDoesNotPoisonConsumerBit)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_h005_poison");

  // TX1: Insert 900 entries to fill most of the per-block budget (max=1000).
  std::vector<QueueEntry> entries_900;
  entries_900.reserve(900);
  for (int i = 0; i < 900; ++i) {
    entries_900.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta(entries_900),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "TX1 insert 900 should succeed";
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // TX2: Consume + insert 200 entries. Aggregate reservation 900 + 200 = 1100
  // exceeds the 1000-entry per-block cap, so push must fail with kConflict.
  std::vector<QueueEntry> entries_200;
  entries_200.reserve(200);
  for (int i = 0; i < 200; ++i) {
    entries_200.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 100) & 0xFF),
                        static_cast<uint8_t>(((i + 100) >> 8) & 0xFF)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_insert_delta(0, 0, {entries_900[0]},
                                              entries_200),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value())
      << "TX2 consume+insert 200 should fail (aggregate > 1000)";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  // TX3: legitimate pure consume. TX2's failed reservation must not leak the
  // consumed_ bit, so TX3 must succeed.
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_consume_delta(0, 0, {entries_900[0]}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "TX3 pure consume must succeed after TX2's failed overfull "
         "reservation";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}

// Sibling leak: a consume that trips kInvalidExecutorBehavior after the
// consumed_ CAS must not leak the single-consumer bit to a subsequent
// legitimate consume in the same block.
TEST_F(DirectRCQTests, InvalidExecutorConsumeDoesNotPoisonConsumerBit)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_invalid_leak_cbit");

  // TX1: insert one entry so TX3 has something to consume.
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // TX2: consume with max_fully_consumed_block_number = 1 on a fresh RCQ
  // (max_live_block_num = 0). This trips
  // retrocausal_queue_lock.cc line ~197 and returns kInvalidExecutorBehavior
  // AFTER consumed_ was already CASed to true.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  // kInvalidExecutorBehavior surfaces as !r.has_value(), distinct from
  // kConflict.
  ASSERT_FALSE(r.has_value())
      << "TX2 should hit kInvalidExecutorBehavior (past block > max_live)";
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // TX3: legitimate pure consume of the current-block entry. Should succeed
  // once consumed_ is rolled back by the guard.
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "TX3 pure consume must succeed after TX2's kInvalidExecutorBehavior";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}

// Sibling leak: a consume+insert that trips kInvalidExecutorBehavior after
// the entry-count fetch_add must not leak the reservation.
TEST_F(DirectRCQTests,
       InvalidExecutorConsumeInsertDoesNotPoisonEntryReservation)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_invalid_leak_res");

  // TX1: reserve 500 entries.
  std::vector<QueueEntry> entries_500;
  entries_500.reserve(500);
  for (int i = 0; i < 500; ++i) {
    entries_500.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta(entries_500),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // TX2: consume_insert with 400 entries + invalid consume. Reservation
  // 500 + 400 = 900 passes the 1000 cap, then kInvalidExecutorBehavior
  // fires in the consume-validation block, leaking the 400-entry reservation.
  std::vector<QueueEntry> entries_400;
  entries_400.reserve(400);
  for (int i = 0; i < 400; ++i) {
    entries_400.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 200) & 0xFF),
                        static_cast<uint8_t>(((i + 200) >> 8) & 0xFF)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_insert_delta(1, 0, {}, entries_400),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "TX2 should hit kInvalidExecutorBehavior (past block > max_live)";
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // TX3: legitimate insert of 200 entries. Total 500 + 200 = 700 <= 1000,
  // must succeed. With the leak it becomes 500 + 400 + 200 = 1100 and fails.
  std::vector<QueueEntry> entries_200;
  entries_200.reserve(200);
  for (int i = 0; i < 200; ++i) {
    entries_200.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 700) & 0xFF),
                        static_cast<uint8_t>(((i + 700) >> 8) & 0xFF)}));
  }
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_insert_delta(entries_200),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "TX3 insert 200 must succeed after TX2's reservation is rolled back";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}

// Stacked sub: sub1 = insert-only, sub2 = consume_insert with invalid
// max_fully_consumed. sub2 should fail at consume validation AFTER the CAS
// and fetch_add have run. ScopedUndo must roll back both so another tx in
// the same block can legitimately consume, and the per-block reservation
// only reflects sub1's inserts.
TEST_F(DirectRCQTests, StackedInsertThenInvalidConsumeInsertDoesNotPoison)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_stacked_invalid");

  std::vector<QueueEntry> entries_100;
  entries_100.reserve(100);
  for (int i = 0; i < 100; ++i) {
    entries_100.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  std::vector<QueueEntry> entries_300;
  entries_300.reserve(300);
  for (int i = 0; i < 300; ++i) {
    entries_300.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: insert-only, 100 entries. reservation becomes 100.
  tx1.add_delta(key, make_rcq_insert_delta(entries_100),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 insert must succeed";

  // Sub2: consume_insert cumulative with invalid max_fully_consumed=1 on a
  // fresh RCQ (max_live_block_num=0). Cumulative inserts = 300 (first 100
  // are sub1's, narrow's prefix check passes). Must fail at lock-time
  // consume validation.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {}, entries_300),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "sub2 should fail with kInvalidExecutorBehavior (max_fully > max_live)";

  // Commit tx1. Sub2 never pushed, so only sub1's work persists.
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // A separate tx consumes one of sub1's current-block entries. Succeeds
  // iff sub2's CAS undo restored consumed_ to false.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0, {entries_100[0]}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 pure consume must succeed; sub2 must not leak consumed_";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  // A separate tx inserts 899 more entries. Succeeds iff the per-block
  // reservation is 100 (sub1 only). With a leaked fetch_add, reservation
  // would be 300 → 300+899>1000 → kConflict.
  std::vector<QueueEntry> entries_899;
  entries_899.reserve(899);
  for (int i = 0; i < 899; ++i) {
    entries_899.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 100) & 0xFF),
                        static_cast<uint8_t>(((i + 100) >> 8) & 0xFF)}));
  }
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_insert_delta(entries_899),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx3 insert 899 must succeed; sub2 must not leak fetch_add";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}

// Stacked sub: sub1 insert-only, sub2 consume_insert VALID but the
// cumulative insert count pushes the per-block reservation past the cap.
// Different failure leg than StackedInsertThenInvalidConsumeInsert: this
// returns kConflict from the overflow check, not kInvalidExecutorBehavior
// from consume validation. ScopedUndo must still unwind CAS + fetch_add.
TEST_F(DirectRCQTests, StackedInsertThenOverfullConsumeInsertDoesNotPoison)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_stacked_overfull");

  // Filler tx: 800 inserts. Reservation becomes 800.
  std::vector<QueueEntry> entries_filler;
  entries_filler.reserve(800);
  for (int i = 0; i < 800; ++i) {
    entries_filler.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 1000) & 0xFF),
                        static_cast<uint8_t>(((i + 1000) >> 8) & 0xFF)}));
  }
  auto tx_filler = make_new_per_tx_db_proxy();
  tx_filler.add_delta(key, make_rcq_insert_delta(entries_filler),
                      std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_filler.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_filler.commit_transaction({}).has_value());

  std::vector<QueueEntry> entries_100;
  entries_100.reserve(100);
  for (int i = 0; i < 100; ++i) {
    entries_100.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  std::vector<QueueEntry> entries_300;
  entries_300.reserve(300);
  for (int i = 0; i < 300; ++i) {
    entries_300.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: 100 inserts. reservation: 800 -> 900.
  tx1.add_delta(key, make_rcq_insert_delta(entries_100),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 insert must succeed";

  // Sub2: valid consume + 300 cumulative inserts. additional = 200.
  // fetch_add would push reservation to 1100 > 1000 cap. Overflow returns
  // kConflict (r.has_value() && !r.value() pattern).
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(0, 0, {}, entries_300),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value())
      << "sub2 should fail with kConflict (800+100+200 > 1000)";

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // Verify consumed_ was restored: another tx can consume a current-block
  // entry from sub1. With a leaked CAS, this would return kConflict.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0, {entries_100[0]}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 pure consume must succeed; sub2 must not leak consumed_";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

// Stacked sub: sub1 = valid consume_insert (sets consumed_ via CAS),
// sub2 = consume_insert with invalid max_fully_consumed. Because
// this_tx_already_consumed is true in sub2, no CAS runs — only the
// fetch_add undo is in play. consumed_ stays set (sub1 owns it); the
// reservation must reflect only sub1's inserts after sub2 fails.
TEST_F(DirectRCQTests,
       StackedConsumeInsertThenInvalidConsumeInsertDoesNotPoison)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_stacked_already");

  std::vector<QueueEntry> entries_100;
  entries_100.reserve(100);
  for (int i = 0; i < 100; ++i) {
    entries_100.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  std::vector<QueueEntry> entries_300;
  entries_300.reserve(300);
  for (int i = 0; i < 300; ++i) {
    entries_300.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: valid consume_insert, 100 inserts. consumed_=true, reservation=100.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(0, 0, {}, entries_100),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 consume_insert must succeed";

  // Sub2: invalid max_fully_consumed=1, cumulative 300 inserts. narrow
  // passes (consume is still present, monotonic; inserts grew). In the
  // lock: this_tx_already_consumed=true so no new CAS. fetch_add(200)
  // runs, undo pushed. Consume validation trips kInvalidExecutorBehavior.
  // Only the reservation undo fires.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {}, entries_300),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "sub2 should fail with kInvalidExecutorBehavior";

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // Reservation should be 100 (sub1 only) if fetch_add was undone; with a
  // leak it would be 300. Verify by inserting 899 more entries from a
  // separate tx: 100+899=999 <= 1000 passes; 300+899=1199 > 1000 fails.
  std::vector<QueueEntry> entries_899;
  entries_899.reserve(899);
  for (int i = 0; i < 899; ++i) {
    entries_899.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 100) & 0xFF),
                        static_cast<uint8_t>(((i + 100) >> 8) & 0xFF)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta(entries_899),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 insert 899 must succeed; sub2 must not leak fetch_add";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

// Pop of a consume-adding sub: sub1 = insert-only, sub2 = consume_insert
// that adds the consume. Pop sub2 → the lock call has before.consume set
// and after.consume null, triggering the else-if CLEAR in
// try_atomic_change_lock. This is the reachable half of that branch;
// its undo path is unreachable today (documented inline).
TEST_F(DirectRCQTests, PopConsumeAddingSubReleasesConsumerBit)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_pop_adds_consume");

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: insert-only, one entry A.
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 insert must succeed";

  // Sub2: consume_insert cumulative. Adds a consume + one more insert so
  // that sub2's cumulative inserts (2) are a superset of sub1's (1) and
  // the prefix check at lock line ~160 passes. Consume is valid (max_fully
  // = 0 on a fresh RCQ). consumed_ becomes true.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(
                    0, 0, {}, {make_rcq_entry({0}), make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub2 consume_insert must succeed";

  // Pop sub2: lock call sees before.consume != null, after.consume null,
  // this_tx_already_consumed = true. else-if CLEAR stores consumed_ =
  // false. Shrink branch fetch_sub(1). Final success → undo disarm.
  auto pop_r = tx1.pop_subtransactions(1);
  ASSERT_TRUE(pop_r.has_value()) << "pop_subtransactions must succeed";

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // A separate tx consumes entry A from current block. With the else-if
  // CLEAR working, consumed_ was released and the CAS here succeeds.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 pure consume must succeed; else-if CLEAR must have released "
         "consumed_";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)
} // namespace hogd
