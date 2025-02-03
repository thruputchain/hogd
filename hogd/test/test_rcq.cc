#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/shared_util/load_contracts.h"
#include "hogd/test/local_test_harness.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Uses the direct coordinator API, which is annoying.
// TODO(geoff): wrap in a nicer API for coord
class TestRCQ : public ::testing::Test, public test::LocalTestHarness {

protected:
  void SetUp() override
  {
    auto [h, c] = load_built_rs_contract("rcq_test.wasm");

    db_core_.get_root_db().write_contract(h, c);
    db_core_.get_root_db().write_deployment(addr_, h);

    syscall_logfd = 1;
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(TestRCQ, ReadSelfItem)
{
  // Call test_read_self_item, which simply writes an item to the queue and
  // immediately reads it back.

  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  exec_client_.open_proposal_block(1)();

  SignedTransaction write_tx;
  write_tx.tx.invocation = {.invoked_address = addr_,
                            .method = compute_sdk_name("test_read_self_item"),
                            .gas_limit = UINT64_MAX,
                            .calldata = make_calldata()};

  auto res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  // reading self inserts does not create proposal data
  auto const &proposal_data = res.proposal_data();
  ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0);

  // assert that two consumes conflict
  res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_NE(res.error(), RPCError::kNone);

  exec_client_.finish_block(1)();

  exec_client_.finish_block(1)();
  db_client_.close_snapshot(1)();
  db_client_.commit_snapshot(1)();
}

TEST_F(TestRCQ, ConsumeAcrossBlocks)
{
  // populate with two blocks of elements
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  exec_client_.open_proposal_block(1)();

  SignedTransaction write_tx;
  write_tx.tx.invocation = {.invoked_address = addr_,
                            .method = compute_sdk_name("make_several_inserts"),
                            .gas_limit = UINT64_MAX,
                            .calldata = make_calldata(
                                std::vector<uint8_t>{0, 1, 2, 3, 4, 5})};

  auto res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  exec_client_.finish_block(1)();
  db_client_.close_snapshot(1)();
  db_client_.commit_snapshot(1)();

  ASSERT_EQ(db_client_.open_database_snapshot(1, 2)(), RPCError::kNone);
  exec_client_.open_proposal_block(2)();

  write_tx.tx.invocation.calldata =
      make_calldata(std::vector<uint8_t>{6, 7, 8, 9, 10});

  res = exec_client_.propose_transaction(2, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  exec_client_.finish_block(2)();
  db_client_.close_snapshot(2)();
  db_client_.commit_snapshot(2)();

  // consume part of the first block of elements
  ASSERT_EQ(db_client_.open_database_snapshot(2, 3)(), RPCError::kNone);
  exec_client_.open_proposal_block(3)();

  SignedTransaction consume_tx;
  consume_tx.tx.invocation = {.invoked_address = addr_,
                              .method =
                                  compute_sdk_name("expect_several_consumes"),
                              .gas_limit = UINT64_MAX,
                              .calldata =
                                  make_calldata(std::vector<uint8_t>{0, 1, 2})};

  res = exec_client_.propose_transaction(3, consume_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);
  {
    auto const &proposal_data = res.proposal_data();
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0);
  }

  exec_client_.finish_block(3)();
  db_client_.close_snapshot(3)();
  db_client_.commit_snapshot(3)();

  // consume the rest of the first block and part of the second
  ASSERT_EQ(db_client_.open_database_snapshot(3, 4)(), RPCError::kNone);
  exec_client_.open_proposal_block(4)();

  consume_tx.tx.invocation.calldata =
      make_calldata(std::vector<uint8_t>{3, 4, 5, 6, 7});

  res = exec_client_.propose_transaction(4, consume_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  {
    auto const &proposal_data = res.proposal_data();
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 0);
  }

  exec_client_.finish_block(4)();
  db_client_.close_snapshot(4)();
  db_client_.commit_snapshot(4)();

  // consume the rest of the second block, and some concurrent elements
  ASSERT_EQ(db_client_.open_database_snapshot(4, 5)(), RPCError::kNone);
  exec_client_.open_proposal_block(5)();

  consume_tx.tx.invocation.calldata =
      make_calldata(std::vector<uint8_t>{8, 9, 10, 11, 12});

  write_tx.tx.invocation.calldata = make_calldata(std::vector<uint8_t>{11, 12});
  res = exec_client_.propose_transaction(5, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  res = exec_client_.propose_transaction(5, consume_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);
  {
    auto const &proposal_data = res.proposal_data();
    ASSERT_EQ(proposal_data.observed_queue_entry_sequence.size(), 2);
    EXPECT_EQ(proposal_data.observed_queue_entry_sequence[0], QueueEntry{11});
    EXPECT_EQ(proposal_data.observed_queue_entry_sequence[1], QueueEntry{12});
  }

  exec_client_.finish_block(5)();
  db_client_.close_snapshot(5)();
  db_client_.commit_snapshot(5)();
}

TEST_F(TestRCQ, ConcurrentConsume)
{
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  exec_client_.open_proposal_block(1)();

  SignedTransaction write_tx;
  write_tx.tx.invocation = {.invoked_address = addr_,
                            .method = compute_sdk_name("make_several_inserts"),
                            .gas_limit = UINT64_MAX,
                            .calldata = make_calldata(
                                std::vector<uint8_t>{0, 1, 2, 3, 4, 5})};

  auto res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  SignedTransaction consume_tx;
  consume_tx.tx.invocation = {.invoked_address = addr_,
                              .method =
                                  compute_sdk_name("expect_several_consumes"),
                              .gas_limit = UINT64_MAX,
                              .calldata = make_calldata(
                                  std::vector<uint8_t>{0, 1, 2, 3})};

  res = exec_client_.propose_transaction(1, consume_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  exec_client_.finish_block(1)();
  ASSERT_EQ(db_client_.close_snapshot(1)(), RPCError::kNone);
  db_client_.commit_snapshot(1)();

  db_client_.open_database_snapshot(1, 2)();
  exec_client_.open_proposal_block(2)();

  consume_tx.tx.invocation.calldata = make_calldata(std::vector<uint8_t>{4, 5});

  res = exec_client_.propose_transaction(2, consume_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  exec_client_.finish_block(2)();
  ASSERT_EQ(db_client_.close_snapshot(2)(), RPCError::kNone);
  db_client_.commit_snapshot(2)();
}

TEST_F(TestRCQ, ReadWeirdOrder)
{
  // prep_for_test_read_weird_order
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  exec_client_.open_proposal_block(1)();

  SignedTransaction prep_tx;
  prep_tx.tx.invocation = {.invoked_address = addr_,
                           .method = compute_sdk_name(
                               "prep_for_test_read_weird_order"),
                           .gas_limit = UINT64_MAX,
                           .calldata = make_calldata()};

  auto res = exec_client_.propose_transaction(1, prep_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  // test_read_weird_order

  SignedTransaction read_tx;
  read_tx.tx.invocation = {.invoked_address = addr_,
                           .method = compute_sdk_name("test_read_weird_order"),
                           .gas_limit = UINT64_MAX,
                           .calldata = make_calldata()};

  res = exec_client_.propose_transaction(1, read_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);
  auto proposal_data = res.proposal_data();

  exec_client_.finish_block(1)();
  db_client_.close_snapshot(1)();

  // now replay with validation mode
  ASSERT_EQ(db_client_.open_database_snapshot(0, 2)(), RPCError::kNone);
  exec_client_.open_validation_block(2)();

  ProposedTransaction proposed_tx{.signed_tx = read_tx,
                                  .proposal_data = proposal_data};

  auto validate_res = exec_client_.validate_transaction(2, proposed_tx)();
  ASSERT_EQ(validate_res, RPCError::kNone);

  // we consumed entries we did not insert.
  EXPECT_NE(db_client_.close_snapshot(2)(), RPCError::kNone);

  validate_res = exec_client_.validate_transaction(
      2, ProposedTransaction{.signed_tx = prep_tx, .proposal_data = {}})();
  ASSERT_EQ(validate_res, RPCError::kNone);

  EXPECT_EQ(db_client_.close_snapshot(2)(), RPCError::kNone);

  exec_client_.finish_block(2)();
}

} // namespace hogd
