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
          false, BlockExecutionMode::kBlockExecutionModeProposal, TestDB::kHogDB)
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

  constexpr static auto make_rcq_insert_delta(std::vector<QueueEntry> const &values)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    delta.retrocausal_queue().insert = RetrocausalQueueInsertDelta{.added_entries = {values.begin(), values.end()}};
    return delta;
  }

  constexpr static auto make_rcq_consume_delta(uint32_t max_fully_consumed_block_number, uint32_t partial_consumed_number, x23::Vec<QueueEntry, 0xffffffff> const &current_block_consumed)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
    RetrocausalQueuePastBlockConsumeDelta consume_delta = {
      .max_fully_consumed_block_number = max_fully_consumed_block_number,
      .partial_consumed_number = partial_consumed_number
    };
    delta.retrocausal_queue().consume = RetrocausalQueueConsumeDelta{.past_block_consume = consume_delta,
    .current_block_consumed = current_block_consumed};
    return delta;
  }

  std::optional<QueueEntry> load_concurrent_entry(AddressAndKey const& addrkey, uint32_t index) {
    auto& proxy = get_db_proxy();
    auto res = proxy.get_concurrent_retrocausal_queue_entry(get_block_number(), addrkey, index);
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
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({1})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConcurrentConsume)
{
    GTEST_SKIP() << "Pending TODO at retrocausal_queue_lock.cc line 77";
    return;

    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({1})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    auto entry1 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
    ASSERT_FALSE(entry1.has_value());

    ASSERT_TRUE(tx1.commit_transaction({}).has_value());

    entry1 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
    ASSERT_TRUE(entry1.has_value());
    ASSERT_EQ(entry1.value(), make_rcq_entry({0}));

    auto entry2 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 1);
    ASSERT_FALSE(entry2.has_value());

    ASSERT_TRUE(tx2.commit_transaction({}).has_value());

    entry2 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 1);
    ASSERT_TRUE(entry2.has_value());
    ASSERT_EQ(entry2.value(), make_rcq_entry({1}));
}

TEST_F(DirectRCQTests, ConcurrentConsumesDisallowed)
{
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // This does not arise when proposing a new block but could arise when validating another node's proposal
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    auto tx3 = make_new_per_tx_db_proxy();
    tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx3.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && !r.value()); // double consume fails

    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
    ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConcurrentConsumesDisallowed2)
{
    GTEST_SKIP() << "Pending TODO at retrocausal_queue_lock.cc line 77";
    return;

    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // This does not arise when proposing a new block but could arise when validating another node's proposal
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    auto tx3 = make_new_per_tx_db_proxy();
    tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx3.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && !r.value()); // double consume fails
    ASSERT_TRUE(tx3.abort_transaction().has_value());

    ASSERT_TRUE(tx1.commit_transaction({}).has_value());
    ASSERT_TRUE(tx2.abort_transaction().has_value());

    // second consume can be reserved if the first is aborted
    auto tx4 = make_new_per_tx_db_proxy();
    tx4.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx4.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    ASSERT_TRUE(tx4.commit_transaction({}).has_value());
}

TEST_F(DirectRCQTests, ConsumeExclusiveLock)
{
    GTEST_SKIP() << "Pending TODO at exclusive_lock.cc line 33";
    return;

    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());


    // then upgrade tx1 to exclusive lock by changing the delta type
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
        make_bytestring_delta("value"), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // another tx cannot concurrently insert
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({1})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && !r.value());
    ASSERT_TRUE(tx2.abort_transaction().has_value());

    // Another tx cannot concurrently make a bytestring write
    auto tx3 = make_new_per_tx_db_proxy();
    tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_bytestring_delta("value"), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx3.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && !r.value());
    ASSERT_TRUE(tx3.abort_transaction().has_value());

    // tx1 can revert back to treating the object as an rcq
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({1})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx1.commit_transaction({}).has_value());

    // Now we can retrieve the retrocausal entry
    auto entry = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry.value(), make_rcq_entry({1}));

    auto entry2 = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 1);
    ASSERT_TRUE(entry2.has_value());
    ASSERT_FALSE(entry2.has_value());
}

TEST_F(DirectRCQTests, RevertFromExclusiveLock) 
{
    GTEST_SKIP() << "Pending TODO at exclusive_lock.cc line 33";
    return;

    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    
    auto tx1 = make_new_per_tx_db_proxy();
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    auto r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // then upgrade tx1 to exclusive lock by changing the delta type
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_bytestring_delta("value"), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());

    // concurrent tx cannot insert right now
    auto tx2 = make_new_per_tx_db_proxy();
    tx2.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({1})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx2.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && !r.value());
    ASSERT_TRUE(tx2.abort_transaction().has_value());

    // tx1 can revert back to treating the object as an rcq
    tx1.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_consume_delta(0, 0, {}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx1.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx1.commit_transaction({}).has_value());

    // concurrent tx can insert right now
    auto tx3 = make_new_per_tx_db_proxy();
    tx3.add_delta(make_contract_data_addrkey(kContractAddress, "rcq0"), 
    make_rcq_insert_delta({make_rcq_entry({0})}), std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
    r = tx3.try_push_current_subtransaction();
    ASSERT_TRUE(r.has_value() && r.value());
    ASSERT_TRUE(tx3.commit_transaction({}).has_value());

    auto entry = load_concurrent_entry(make_contract_data_addrkey(kContractAddress, "rcq0"), 0);
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry.value(), make_rcq_entry({0}));
}

// NOLINTEND(bugprone-unchecked-optional-access)
} // namespace hogd
