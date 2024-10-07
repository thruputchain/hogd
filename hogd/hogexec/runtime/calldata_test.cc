#include <gtest/gtest.h>

#include <memory>
#include <cstdint>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"

#include "types.hh"
#include "contract.hh"
#include "transaction.hh"

namespace hogd {

class CalldataTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // this wasm just invokes an external call.  We're just reusing it here.
    auto c = test::load_built_rs_contract("test_calldata.wasm");

    auto contract_hash = storage_snapshot_.register_contract(c);
    storage_snapshot_.deploy_contract(addr_, contract_hash);
  }

  test::LocalMockStorageSnapshot storage_snapshot_;
  ContractAddress addr_ = ContractAddress{};
};

TEST_F(CalldataTest, NoCalldata)
{

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;

  SystemCallHandler handler(stx, storage_snapshot_);

  handler.root_invoke();
}


}
