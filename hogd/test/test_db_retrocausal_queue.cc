#include <gtest/gtest.h>

#include "hogd/test/local_test_harness.h"

namespace hogd {

class TestDbRetrocausalQueue : public ::testing::Test,
                               public test::DatabaseWrapper {
protected:
  void SetUp() override { test::DatabaseWrapper::initialize(); }

  constexpr static RPCFlagSet kFlags =
      std::to_underlying(RPCFlags::kFinalCommit);

  static QueueEntry make_queue_entry(uint64_t value)
  {
    QueueEntry entry;
    entry.resize_uninitialized(sizeof(value));
    std::memcpy(entry.data(), &value, sizeof(value));
    return entry;
  }

  static StorageDelta make_insert_delta(std::vector<uint64_t> const &values)
  {
    StorageDelta delta(StorageDeltaType::kRetrocausalQueueModify);

    for (auto value : values) {
      delta.retrocausal_queue().insert.added_entries.push_back(
          make_queue_entry(value));
    }
    return delta;
  };

  static StorageDelta make_consume_delta(
      RetrocausalQueuePastBlockConsumeDelta const &past_block_consume,
      std::vector<uint64_t> const &indices)
  {
    StorageDelta delta(StorageDeltaType::kRetrocausalQueueModify);
    delta.retrocausal_queue().consume.emplace(
        RetrocausalQueueConsumeDelta{.past_block_consume = past_block_consume});
    for (auto index : indices) {
      delta.retrocausal_queue().consume->current_block_consumed.push_back(
          make_queue_entry(index));
    }

    return delta;
  }
};

TEST_F(TestDbRetrocausalQueue, OneBlockInsertConsume)
{
  ASSERT_EQ(coord_client_->open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(exec_client_->try_reserve_delta(1, 1, AddressAndKey{},
                                            make_insert_delta({1}), kFlags)(),
            RPCError::kNone);

  auto res =
      exec_client_->get_concurrent_retrocausal_queue_entry(1, AddressAndKey{},
                                                           0)();
  ASSERT_EQ(res.error(), RPCError::kNoRetrocausalQueueItem);

  exec_client_->commit_transaction(1, 1)();

  res = exec_client_->get_concurrent_retrocausal_queue_entry(1, AddressAndKey{},
                                                             0)();
  ASSERT_EQ(res.error(), RPCError::kNone);
  ASSERT_EQ(res.entry(), make_queue_entry(1));

  res = exec_client_->get_concurrent_retrocausal_queue_entry(1, AddressAndKey{},
                                                             1)();
  ASSERT_EQ(res.error(), RPCError::kNoRetrocausalQueueItem);

  // Now consume the entry
  ASSERT_EQ(exec_client_->try_reserve_delta(
                1, 2, AddressAndKey{},
                make_consume_delta(RetrocausalQueuePastBlockConsumeDelta{0, 0},
                                   {1}),
                kFlags)(),
            RPCError::kNone);

  exec_client_->commit_transaction(1, 2)();

  coord_client_->close_snapshot(1)();

  // assert the entry was consumed, and not sent to the next block
  ASSERT_EQ(coord_client_->open_database_snapshot(1, 2)(), RPCError::kNone);

  res = exec_client_->get_concurrent_retrocausal_queue_entry(2, AddressAndKey{},
                                                             0)();
  ASSERT_EQ(res.error(), RPCError::kNoRetrocausalQueueItem);

  auto past_block_res =
      exec_client_->get_next_retrocausal_queue_block(2, AddressAndKey{}, 0)();
  ASSERT_EQ(past_block_res.error(), RPCError::kNoRetrocausalQueueItem);

  coord_client_->close_snapshot(2)();
}

TEST_F(TestDbRetrocausalQueue, MultiBlockInsertConsume)
{

  ASSERT_EQ(coord_client_->open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(exec_client_->try_reserve_delta(1, 1, AddressAndKey{},
                                            make_insert_delta({1, 2, 3, 4}),
                                            kFlags)(),
            RPCError::kNone);

  auto res =
      exec_client_->get_concurrent_retrocausal_queue_entry(1, AddressAndKey{},
                                                           0)();
  ASSERT_EQ(res.error(), RPCError::kNoRetrocausalQueueItem);

  exec_client_->commit_transaction(1, 1)();

  auto check_entry = [&](BlockID block_id, uint64_t index, uint64_t value) {
    res = exec_client_->get_concurrent_retrocausal_queue_entry(block_id,
                                                               AddressAndKey{},
                                                               index)();
    ASSERT_EQ(res.error(), RPCError::kNone);
    ASSERT_EQ(res.entry(), make_queue_entry(value));
  };

  check_entry(1, 0, 1);
  check_entry(1, 1, 2);
  check_entry(1, 2, 3);
  check_entry(1, 3, 4);

  // consume some of the entries

  ASSERT_EQ(exec_client_->try_reserve_delta(
                1, 2, AddressAndKey{},
                make_consume_delta(RetrocausalQueuePastBlockConsumeDelta{0, 0},
                                   {1, 3}),
                kFlags)(),
            RPCError::kNone);

  // double consume not allowed
  ASSERT_NE(exec_client_->try_reserve_delta(
                1, 3, AddressAndKey{},
                make_consume_delta(RetrocausalQueuePastBlockConsumeDelta{0, 0},
                                   {2, 4}),
                kFlags)(),
            RPCError::kNone);

  exec_client_->commit_transaction(1, 2)();
  exec_client_->abort_transaction(1, 3)();

  coord_client_->close_snapshot(1)();
  // Remaining should be 2, 4

  ASSERT_EQ(coord_client_->open_database_snapshot(1, 2)(), RPCError::kNone);

  auto past_block_res =
      exec_client_->get_next_retrocausal_queue_block(2, AddressAndKey{}, 0)();
  ASSERT_EQ(past_block_res.error(), RPCError::kNone);
  ASSERT_EQ(past_block_res.block().block_number, 1);
  ASSERT_EQ(past_block_res.block().entries.size(), 2);
  ASSERT_EQ(past_block_res.block().entries[0], make_queue_entry(2));
  ASSERT_EQ(past_block_res.block().entries[1], make_queue_entry(4));

  // consume from past entries
  ASSERT_EQ(exec_client_->try_reserve_delta(
                2, 4, AddressAndKey{},
                make_consume_delta(RetrocausalQueuePastBlockConsumeDelta{0, 1},
                                   {}),
                kFlags)(),
            RPCError::kNone);

  exec_client_->commit_transaction(2, 4)();

  coord_client_->close_snapshot(2)();

  coord_client_->open_database_snapshot(2, 3)();

  past_block_res =
      exec_client_->get_next_retrocausal_queue_block(3, AddressAndKey{}, 0)();
  ASSERT_EQ(past_block_res.error(), RPCError::kNone);
  ASSERT_EQ(past_block_res.block().block_number, 1);
  ASSERT_EQ(past_block_res.block().entries.size(), 1);
  ASSERT_EQ(past_block_res.block().entries[0], make_queue_entry(4));
  coord_client_->close_snapshot(3)();
}

} // namespace hogd
