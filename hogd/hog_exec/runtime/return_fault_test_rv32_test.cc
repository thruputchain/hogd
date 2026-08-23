#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/return_fault_test.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ReturnFaultTestRv32Test : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  ReturnFaultTestRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb),
      runtime_context_(make_hog_exec_context())
  {}

protected:
  void SetUp() override
  {
    using return_fault_test_meta =
        ContractTraitMeta<return_fault_test::ReturnFaultTest>;

    // Load contract
    auto contract = return_fault_test_meta::load_rv32();

    // Deploy contract
    write_contract(contract);
    write_deployment(kContractAddress, contract);

    // Grant storage tokens
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<return_fault_test::ReturnFaultTest> client_ =
      ContractTraitClient<return_fault_test::ReturnFaultTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;

  uint64_t nonce_ = 0;
};

TEST_F(ReturnFaultTestRv32Test, InvokeRunTest)
{
  // Invoke the run_test method
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"run_test">(UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
}

} // namespace hogd
