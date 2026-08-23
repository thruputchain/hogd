#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/nnint_output_fault_test.h"
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

class NnintOutputFaultTestRv32Test : public ::testing::Test,
                                     public NativeContractTestFramework {
public:
  NnintOutputFaultTestRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb),
      runtime_context_(make_hog_exec_context())
  {}

protected:
  void SetUp() override
  {
    using nnint_output_fault_test_meta =
        ContractTraitMeta<nnint_output_fault_test::NnintOutputFaultTest>;

    auto contract = nnint_output_fault_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<nnint_output_fault_test::NnintOutputFaultTest> client_ =
      ContractTraitClient<nnint_output_fault_test::NnintOutputFaultTest>(
          kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;

  uint64_t nonce_ = 0;
};

// Valid first output pointer, invalid second output pointer.
// Asserts that the valid first output is not overwritten on fault.
TEST_F(NnintOutputFaultTestRv32Test, FaultValidFirst)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_fault_valid_first">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
}

// Invalid first output pointer, valid second output pointer.
// Asserts that the valid second output is not overwritten on fault.
TEST_F(NnintOutputFaultTestRv32Test, FaultInvalidFirst)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_fault_invalid_first">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
}

TEST_F(NnintOutputFaultTestRv32Test, ValidationFailure)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_validation_failure">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
}

} // namespace hogd
