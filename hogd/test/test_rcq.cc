#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/shared_util/load_contracts.h"
#include "hogd/test/local_test_harness.h"
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
  // assert that two consumes conflict
  res = exec_client_.propose_transaction(1, write_tx)();
  ASSERT_NE(res.error(), RPCError::kNone);

  // TODO(geoff): check NondeterministicProposalData.

  exec_client_.finish_block(1)();
  db_client_.close_snapshot(1)();
  db_client_.commit_snapshot(1)();
}

} // namespace hogd
