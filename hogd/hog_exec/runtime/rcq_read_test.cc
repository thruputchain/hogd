#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class RCQReadTest : public ::testing::TestWithParam<TestDB>,
                    public NativeContractTestFramework {
public:
  RCQReadTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }

  static constexpr ContractAddress kContractAddress = make_contract_address(0);

  static constexpr QueueEntry make_rcq_entry(uint8_t value)
  {
    return QueueEntry{value};
  }

  static StorageDelta
  make_rcq_insert_delta(std::vector<QueueEntry> const &values)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{
        .added_entries = {values.begin(), values.end()}};
    return delta;
  }

  static StorageDelta
  make_rcq_consume_delta(uint64_t max_fully_consumed_block_number,
                         uint64_t partial_consumed_number)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().consume = RetrocausalQueueConsumeDelta{
        .past_block_consume =
            RetrocausalQueuePastBlockConsumeDelta{
                .max_fully_consumed_block_number =
                    max_fully_consumed_block_number,
                .partial_consumed_number = partial_consumed_number},
        .current_block_consumed = {}};
    return delta;
  }

  static StorageDelta
  make_rcq_consume_insert_delta(uint64_t max_fully_consumed_block_number,
                                uint64_t partial_consumed_number,
                                std::vector<QueueEntry> const &values)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().consume = RetrocausalQueueConsumeDelta{
        .past_block_consume =
            RetrocausalQueuePastBlockConsumeDelta{
                .max_fully_consumed_block_number =
                    max_fully_consumed_block_number,
                .partial_consumed_number = partial_consumed_number},
        .current_block_consumed = {}};
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{
        .added_entries = {values.begin(), values.end()}};
    return delta;
  }

  void submit_delta(AddressAndKey const &key, StorageDelta delta)
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(key, std::move(delta),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
    advance_block();
  }
};
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_P(RCQReadTest, ExactLookupDoesNotReturnBlockForZero)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(9)}));

  auto block_from_zero =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      0);
  ASSERT_TRUE(block_from_zero.has_value());
  ASSERT_FALSE(block_from_zero.value().has_value());

  auto block_from_one =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block_from_one.has_value());
  ASSERT_TRUE(block_from_one.value().has_value());
  ASSERT_EQ(block_from_one.value()->block_number, 1);
  ASSERT_EQ(block_from_one.value()->entries.size(), 1);
  ASSERT_EQ(block_from_one.value()->entries[0], make_rcq_entry(9));
}

TEST_P(RCQReadTest, ExactLookupDoesNotSkipFullyConsumedBlocks)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2

  // Consume block 1 fully.
  submit_delta(key, make_rcq_consume_delta(1, 0));

  auto block_from_zero =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      0);
  ASSERT_TRUE(block_from_zero.has_value());
  ASSERT_FALSE(block_from_zero.value().has_value());

  auto block_from_one =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block_from_one.has_value());
  ASSERT_FALSE(block_from_one.value().has_value());

  auto block_from_two =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(block_from_two.has_value());
  ASSERT_TRUE(block_from_two.value().has_value());
  EXPECT_EQ(block_from_two.value()->block_number, 2);
  ASSERT_EQ(block_from_two.value()->entries.size(), 1);
  EXPECT_EQ(block_from_two.value()->entries[0], make_rcq_entry(2));
}

TEST_P(RCQReadTest, ExactLookupAfterDeleteAndRecreate)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2
  submit_delta(key, make_delete_delta());                        // block 3
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(9)})); // block 4

  auto block_from_zero =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      0);
  ASSERT_TRUE(block_from_zero.has_value());
  ASSERT_FALSE(block_from_zero.value().has_value());

  auto block_from_one =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block_from_one.has_value());
  ASSERT_TRUE(block_from_one.value().has_value());
  ASSERT_EQ(block_from_one.value()->entries.size(), 1);
  EXPECT_EQ(block_from_one.value()->entries[0], make_rcq_entry(9));
}

TEST_P(RCQReadTest, DeleteRecreateClearsPriorExactBlockLookup)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2
  submit_delta(key, make_delete_delta());                        // block 3
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(9)})); // block 4

  auto block_from_two =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(block_from_two.has_value());
  EXPECT_FALSE(block_from_two.value().has_value());
}

TEST_P(RCQReadTest, ConsumeRecreateClearsPriorExactBlockLookup)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2
  submit_delta(key, make_rcq_consume_delta(2, 0));               // block 3
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(9)})); // block 4

  auto block_from_two =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(block_from_two.has_value());
  EXPECT_FALSE(block_from_two.value().has_value());
}

TEST_P(RCQReadTest, ConsumeInsertClearsPriorExactBlockLookup)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2
  submit_delta(key,
               make_rcq_consume_insert_delta(2, 0,
                                             {make_rcq_entry(9)})); // block 3

  auto block_from_two =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(block_from_two.has_value());
  EXPECT_FALSE(block_from_two.value().has_value());
}

TEST_P(RCQReadTest, ConcurrentConsumeAndInsertClearPriorExactBlockLookup)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2

  // Same block: one tx consumes all prior entries, one tx inserts.
  auto tx_consume = make_new_per_tx_db_proxy();
  tx_consume.add_delta(key, make_rcq_consume_delta(2, 0),
                       std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_consume.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  auto tx_insert = make_new_per_tx_db_proxy();
  tx_insert.add_delta(key, make_rcq_insert_delta({make_rcq_entry(9)}),
                      std::to_underlying(StorageDeltaFlags::None));
  r = tx_insert.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  ASSERT_TRUE(tx_consume.commit_transaction().has_value());
  ASSERT_TRUE(tx_insert.commit_transaction().has_value());
  advance_block();

  auto block_from_two =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      2);
  ASSERT_TRUE(block_from_two.has_value());
  EXPECT_FALSE(block_from_two.value().has_value());
}

TEST_P(RCQReadTest, ExactLookupWithManyInsertAndConsumeBlocks)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_read");

  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(1)})); // block 1
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(2)})); // block 2
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(3)})); // block 3
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(4)})); // block 4
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(5)})); // block 5
  submit_delta(key, make_rcq_insert_delta({make_rcq_entry(6)})); // block 6

  // Consume the first four blocks across several later blocks.
  submit_delta(key, make_rcq_consume_delta(1, 0));
  submit_delta(key, make_rcq_consume_delta(2, 0));
  submit_delta(key, make_rcq_consume_delta(3, 0));
  submit_delta(key, make_rcq_consume_delta(4, 0));

  auto block_from_zero =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      0);
  ASSERT_TRUE(block_from_zero.has_value());
  ASSERT_FALSE(block_from_zero.value().has_value());

  auto block_from_one =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      1);
  ASSERT_TRUE(block_from_one.has_value());
  ASSERT_FALSE(block_from_one.value().has_value());

  auto block_from_four =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      4);
  ASSERT_TRUE(block_from_four.has_value());
  ASSERT_FALSE(block_from_four.value().has_value());

  auto block_from_five =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      5);
  ASSERT_TRUE(block_from_five.has_value());
  ASSERT_TRUE(block_from_five.value().has_value());
  ASSERT_EQ(block_from_five.value()->entries.size(), 1);
  EXPECT_EQ(block_from_five.value()->entries[0], make_rcq_entry(5));

  auto next_block =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      6);
  ASSERT_TRUE(next_block.has_value());
  ASSERT_TRUE(next_block.value().has_value());
  ASSERT_EQ(next_block.value()->entries.size(), 1);
  EXPECT_EQ(next_block.value()->entries[0], make_rcq_entry(6));

  auto block_from_seven =
      get_db_proxy().get_next_retrocausal_queue_block(get_block_number(), key,
                                                      7);
  ASSERT_TRUE(block_from_seven.has_value());
  ASSERT_FALSE(block_from_seven.value().has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQReadTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
