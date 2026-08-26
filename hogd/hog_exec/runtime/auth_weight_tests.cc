#include <gtest/gtest.h>

// End-to-end rv32 coverage for the auth-configuration syscalls, driven
// through the auth_weight_test contract (local_test_contracts):
// WriteAddressRequiredWeight and GetAuthClaim, which previously had only
// native (host-driven) coverage in syscall_auth_config_test.cc.

#include <cstdint>
#include <memory>

#include "address_auth.hh"
#include "built_contracts/auth_weight_test.h"
#include "exec_result.hh"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class AuthWeightTests : public ::testing::Test,
                        public NativeContractTestFramework {
public:
  AuthWeightTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  using auth_weight_test_meta =
      ContractTraitMeta<auth_weight_test::AuthWeightTest>;

  void SetUp() override
  {
    auto contract = auth_weight_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  // Deploys a second copy of the contract, for the read-only test: the
  // reentrance guard rejects an invoke of a contract already on the
  // invocation stack, so the internal helper has to run at a different
  // address.
  void deploy_target()
  {
    auto contract = auth_weight_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kTargetAddress, contract);
    setup_grant_storage_token(kTargetAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);
  constexpr static ContractAddress kTargetAddress = make_contract_address(1);

  ContractTraitClient<auth_weight_test::AuthWeightTest> client_ =
      ContractTraitClient<auth_weight_test::AuthWeightTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;
};

// A weight written through the rv32 guest path lands in the committed
// AuthData storage item.
TEST_F(AuthWeightTests, WriteWeightCommitsAuthData)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_write_weight">(UINT64_MAX,
                                                                     5U),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  advance_block();

  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(kContractAddress));
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item.value().auth_data().required_weight, 5U);
}

// GetAuthClaim from a contract frame reports the transaction's claim; the
// contract asserts the default claim shape (source-address claim, empty
// bitmap, required weight 0).
TEST_F(AuthWeightTests, GetAuthClaimDefaultClaim)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_get_auth_claim">(
                          UINT64_MAX),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
}

// WriteAddressRequiredWeight is a write syscall: in a read-only child frame
// it fails with kInvalidSyscallInput (asserted inside the contract) and
// leaves no AuthData item behind.
TEST_F(AuthWeightTests, WriteWeightRejectedInReadOnlyFrame)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_write_weight_read_only">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  advance_block();

  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(kTargetAddress));
  EXPECT_FALSE(item.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
