#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "auth_claim.hh"
#include "built_contracts/authclaim_test.h"
#include "built_contracts/contract_loader.h"
#include "exec_result.hh"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class AuthClaimTestFixture : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  AuthClaimTestFixture()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  static std::unique_ptr<RuntimeContext> make_context()
  {
    return make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  };

  using authclaim_client_t = ContractTraitClient<authclaim_test::AuthClaimTest>;

  std::unique_ptr<RuntimeContext> context_;

  void SetUp() override { context_ = make_context(); }

  void setup_contract(authclaim_client_t &client)
  {
    auto contract = hogd::load_contract("authclaim_test.wasm");
    auto contract_addr = client.address();
    write_contract(contract);
    write_deployment(contract_addr, contract);
    setup_grant_storage_token(contract_addr, 1'000'000'000);
  }

  ExecResult run_method(auto invocation, AuthClaim auth_claim = AuthClaim{})
  {
    SignedTransaction stx;
    stx.tx.invocation = std::move(invocation);
    stx.tx.auth_claim = std::move(auth_claim);

    auto handler = make_new_system_call_handler(stx, *context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxDeterministicError;
    }
    return handler.try_commit_transaction();
  }
};

TEST_F(AuthClaimTestFixture, TestGetAuthClaim)
{
  auto contract_addr = make_contract_address(0);
  authclaim_client_t client(contract_addr);
  setup_contract(client);

  // Contract expects allowed_keys == 0 && required_weight == 0
  AuthClaim claim{};
  auto result =
      run_method(client.make_invocation<"test_get_auth_claim">(UINT64_MAX),
                 claim);
  ASSERT_EQ(result, ExecResult::ExecResultOK);
}

TEST_F(AuthClaimTestFixture, TestSetAddressRequiredWeight)
{
  auto contract_addr = make_contract_address(0);
  authclaim_client_t client(contract_addr);
  setup_contract(client);

  auto result = run_method(
      client.make_invocation<"test_set_address_required_weight">(UINT64_MAX));
  ASSERT_EQ(result, ExecResult::ExecResultOK);
}

TEST_F(AuthClaimTestFixture, TestAddAddressPublicKey)
{
  auto contract_addr = make_contract_address(0);
  authclaim_client_t client(contract_addr);
  setup_contract(client);

  auto result = run_method(
      client.make_invocation<"test_add_address_public_key">(UINT64_MAX));
  ASSERT_EQ(result, ExecResult::ExecResultOK);
}

TEST_F(AuthClaimTestFixture, TestRemoveAddressPublicKey)
{
  auto contract_addr = make_contract_address(0);
  authclaim_client_t client(contract_addr);
  setup_contract(client);

  auto result = run_method(
      client.make_invocation<"test_remove_address_public_key">(UINT64_MAX));
  ASSERT_EQ(result, ExecResult::ExecResultOK);
}

TEST_F(AuthClaimTestFixture, TestAddPubkeyIndexOob)
{
  auto contract_addr = make_contract_address(0);
  authclaim_client_t client(contract_addr);
  setup_contract(client);

  auto result = run_method(
      client.make_invocation<"test_add_pubkey_index_oob">(UINT64_MAX));
  ASSERT_EQ(result, ExecResult::ExecResultOK);
}

} // namespace hogd
