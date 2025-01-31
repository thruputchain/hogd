#include <gtest/gtest.h>

#include "hogd/test/local_test_harness.h"

namespace hogd {

class TestDbRetrocausalQueue : public ::testing::Test,
                               public test::DatabaseWrapper {
protected:
  void SetUp() override { test::DatabaseWrapper::initialize(); }

  constexpr static RPCFlagSet kFlags =
      std::to_underlying(RPCFlags::kFinalCommit);
};

TEST_F(TestDbRetrocausalQueue, OneBlockInsertConsume)
{
  auto make_queue_entry = [](uint64_t value) {
    QueueEntry entry;
    entry.resize_uninitialized(sizeof(value));
    std::memcpy(entry.data(), &value, sizeof(value));
    return entry;
  };

  auto make_insert_delta = [&](uint64_t value) {
    StorageDelta delta(StorageDeltaType::kRetrocausalQueueModify);

    delta.retrocausal_queue().insert.added_entries.push_back(
        make_queue_entry(value));
    return delta;
  };

  ASSERT_EQ(coord_client_->open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(exec_client_->try_reserve_delta(1, 1, AddressAndKey{},
                                            make_insert_delta(1), kFlags)(),
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

} // namespace hogd
