#include <gtest/gtest.h>

// Miscellaneous end to end tests on syscall behavior,
// implemented within a wasm contract.
// These tests are just wrappers around the tests in the syscall_misc_tests
// contract.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/syscall_misc_tests.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class SyscallMiscTests : public ::testing::Test,
                         public NativeContractTestFramework {
public:
  SyscallMiscTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  void SetUp() override
  {
    using syscall_misc_tests_meta =
        ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;

    // Load contract
    auto contract = syscall_misc_tests_meta::load_wasm();

    // Deploy contract
    write_contract(contract);
    write_deployment(kContractAddress, contract);

    // Grant storage tokens
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hogexec_context();
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<syscall_misc_tests::SyscallMiscTests> client_ =
      ContractTraitClient<syscall_misc_tests::SyscallMiscTests>(
          kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;

  uint64_t nonce_ = 0;
};

TEST_F(SyscallMiscTests, InvokeTestGetReturnBeforeReturnBufferSet)
{
  // Invoke the run_test method
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation = client_.make_invocation<
                      "test_get_return_before_return_buffer_set">(UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

TEST_F(SyscallMiscTests, InvokeTestGetReturnAfterFault)
{
  // Invoke the run_test method
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      client_.make_invocation<"test_get_return_after_fault">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
