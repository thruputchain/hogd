#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/test/local_test_harness.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class TestBytestringWrite : public ::testing::Test,
                            public test::LocalTestHarness {

protected:
  void SetUp() override
  {
    auto [h, c] = test::load_built_rs_contract("test_bytestring.wasm");

    db_core_.get_root_db().write_contract(h, c);
    db_core_.get_root_db().write_deployment(addr_, h);
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(TestBytestringWrite, ReadExpected)
{
  // Uses the direct coordinator API, which is annoying.
  // TODO(geoff): wrap in a nicer API for coord

  // This test runs two transactions that do not conflict in block 1,
  // plus one that conflicts (and is rejected),
  // then reads the result of the bytestring they wrote in block 2.
  // Block 2 also writes a different bytestring, which should not be visible
  // to the read.
  // We then read the bytestring again in block 3, which should see the
  // bytestring from block 2.

  // Initial committed db snapshot is 0
  // open new db snapshot to start
  ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  exec_client_.open_proposal_block(1)();

  StorageKey key{};

  SignedTransaction write_tx;
  write_tx.tx.invocation = {.invoked_address = addr_,
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = test::make_calldata(key, uint32_t{12})};

  auto res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);
  res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  SignedTransaction write_tx2;
  write_tx2.tx.invocation = {
      .invoked_address = addr_,
      .method = 0,
      .gas_limit = UINT64_MAX,
      .calldata = test::make_calldata(
          key, uint32_t{13})}; // write a different bytestring; will conflict.

  res = exec_client_.propose_transaction(1, write_tx2)();
  ASSERT_EQ(res.error(), RPCError::kTransactionFailed);

  exec_client_.finish_block(1)();
  db_client_.commit_snapshot(1)();

  ASSERT_EQ(db_client_.open_database_snapshot(1, 2)(), RPCError::kNone);

  exec_client_.open_proposal_block(2)();

  res = exec_client_.propose_transaction(2, write_tx2)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  SignedTransaction read_tx;
  read_tx.tx.invocation = {.invoked_address = addr_,
                           .method = 2,
                           .gas_limit = UINT64_MAX,
                           .calldata = test::make_calldata(key, uint32_t{12})};

  res = exec_client_.propose_transaction(2, read_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  exec_client_.finish_block(2)();
  db_client_.commit_snapshot(2)();

  ASSERT_EQ(db_client_.open_database_snapshot(2, 3)(), RPCError::kNone);

  exec_client_.open_proposal_block(3)();

  read_tx.tx.invocation.calldata = test::make_calldata(key, uint32_t{13});
  res = exec_client_.propose_transaction(3, read_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);

  exec_client_.finish_block(3)();
  db_client_.commit_snapshot(3)();
}

} // namespace hogd
