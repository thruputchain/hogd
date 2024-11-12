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

class DeployRevertTest : public ::testing::Test,
                         public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h_test_deploy_revert_1, c_test_deploy_revert_1] =
        test::load_built_rs_contract("test_deploy_revert_1.wasm");

    contract_map_->add_contract(h_test_deploy_revert_1, c_test_deploy_revert_1);
    contract_map_->deploy_contract(addr_, h_test_deploy_revert_1);
    contract_map_->commit_round();
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(DeployRevertTest, AssertRevertedDeploysRevert)
{
  auto [h_echo, c_echo] = test::load_built_rs_contract("echo.wasm");
  auto [h_test_deploy_revert_2, c_test_deploy_revert_2] =
      test::load_built_rs_contract("test_deploy_revert_2.wasm");

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method = test::get_sdk_name("deploy_revert_test_root");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata =
      test::make_calldata(h_echo, h_test_deploy_revert_2);
  stx.tx.contracts_to_deploy.push_back(*c_echo);
  stx.tx.contracts_to_deploy.push_back(*c_test_deploy_revert_2);

  SystemCallHandler handler(stx, *snapshot_cache_);

  ASSERT_TRUE(!!handler.root_invoke().result);

  ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
            StorageRPCError::kNone);
}

} // namespace hogd
