#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/test/local_test_harness.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class TestBytestringWrite : public ::testing::Test,
                            public test::LocalTestHarness {

protected:
  void SetUp() override
  {
    std::printf("starting setup\n");
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

  // Initial db snapshot is 0
  // ASSERT_EQ(db_client_.open_database_snapshot(0, 1)(), RPCError::kNone);
  exec_client_.open_proposal_block(0)();

  std::printf("got here\n");

  StorageKey key{};

  SignedTransaction write_tx;
  write_tx.tx.invocation = {.invoked_address = addr_,
                            .method = 0,
                            .gas_limit = UINT64_MAX,
                            .calldata = test::make_calldata(key, uint32_t{12})};

  auto res = exec_client_.propose_transaction(0, write_tx)();
  ASSERT_EQ(res.error(), RPCError::kNone);
  std::printf("got to end\n");
}

} // namespace hogd
