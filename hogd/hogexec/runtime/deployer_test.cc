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

class DeployerTest : public ::testing::Test, public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();
    test::RuntimeTestFramework::deploy_genesis_block();
  }
};

TEST_F(DeployerTest, DeployAndEcho)
{
  auto [h, c] = test::load_built_rs_contract("echo.wasm");

  ContractAddress deployed_address;

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = kDeployerAddress;
    stx.tx.invocation.method_name = 1;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata =
        test::make_calldata(h, MethodID{0}, uint32_t{0}, uint32_t{8},
                            std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7});
    stx.tx.contracts_to_deploy.push_back(*c);
    stx.tx.nonce = 0;

    SystemCallHandler handler(stx, *snapshot_cache_);
    auto stx_hash = handler.get_source_tx_hash();

    deployed_address =
        SystemCallHandler::static_get_deploy_address(kDeployerAddress, stx_hash,
                                                     h);

    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(handler.try_reserve_transaction(get_write_client()),
              StorageRPCError::kNone);
    handler.commit_transaction(get_write_client());
  }

  commit_round();

  // assert that the contract we deployed in prev block is invocable here

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = deployed_address;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata =
      test::make_calldata(uint32_t{0}, uint32_t{4},
                          std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7});
  stx.tx.nonce = 0;

  SystemCallHandler handler(stx, *snapshot_cache_);

  ASSERT_TRUE(!!handler.root_invoke().result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 1);

  check_log_eq(logs[0], test::make_calldata(std::vector<uint8_t>{0, 1, 2, 3}));
}

} // namespace hogd
