#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "storage_interface.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class ReentrantRevertTest : public ::testing::Test,
                            public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h, c] = test::load_built_rs_contract("test_reentrant_revert.wasm");

    contract_map_->add_contract(h, c);
    contract_map_->deploy_contract(addr_, h);
    contract_map_->commit_round();
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(ReentrantRevertTest, AssertReentrantRevertPoisoning)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method = test::get_sdk_name("root_test_reentrant_revert");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata();

  SystemCallHandler handler(stx, *snapshot_cache_);

  ASSERT_TRUE(!handler.root_invoke().result);
}

} // namespace hogd
