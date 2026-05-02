#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>

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

  // Helper to create authclaims matching the hardcoded values in the contract
  static AuthClaimData make_auth_claims()
  {
    AuthClaimData auth_claims;

    // Index 0: kInvokedAddressSignature with KeyID = 5
    AuthClaim claim0(AuthClaimType::kInvokedAddressSignature);
    claim0.invoked_address_signature_id() = 5;
    auth_claims.push_back(claim0);

    // Index 1: kInvokedAddressSignedWeight with BitMap = 5 (bits 0 and 2),
    // Weight = 10
    AuthClaim claim1(AuthClaimType::kInvokedAddressSignedWeight);
    claim1.invoked_address_signed_weight().accepted_keys =
        5; // bits 0 and 2 set
    claim1.invoked_address_signed_weight().required_weight = 10;
    auth_claims.push_back(claim1);

    // Index 2: kOtherAddressSignature with ContractAddress = [0xAA; 32], KeyID
    // = 7
    AuthClaim claim2(AuthClaimType::kOtherAddressSignature);
    ContractAddress addr_aa{};
    addr_aa.fill(0xAA);
    claim2.other_address_signature_id().address = addr_aa;
    claim2.other_address_signature_id().key_id = 7;
    auth_claims.push_back(claim2);

    // Index 3: kOtherAddressSignedWeight with ContractAddress = [0xBB; 32],
    // BitMap = 3 (bits 0 and 1), Weight = 15
    AuthClaim claim3(AuthClaimType::kOtherAddressSignedWeight);
    ContractAddress addr_bb{};
    addr_bb.fill(0xBB);
    claim3.other_address_signed_weight().address = addr_bb;
    claim3.other_address_signed_weight().accepted_keys = 3; // bits 0 and 1 set
    claim3.other_address_signed_weight().required_weight = 15;
    auth_claims.push_back(claim3);

    return auth_claims;
  }

  std::unique_ptr<RuntimeContext> context_;

  void SetUp() override { context_ = make_context(); }
};

TEST_F(AuthClaimTestFixture, TestInvokedAddressSignature)
{
  auto contract = hogd::load_contract("authclaim_test.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  authclaim_client_t client(contract_addr);
  auto auth_claims = make_auth_claims();

  {
    SignedTransaction stx;
    stx.tx.invocation =
        client.make_invocation<"test_invoked_address_signature">(UINT64_MAX);
    stx.tx.auth_claims = auth_claims;

    auto handler = make_new_system_call_handler(stx, *context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

TEST_F(AuthClaimTestFixture, TestInvokedAddressSignedWeight)
{
  auto contract = hogd::load_contract("authclaim_test.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  authclaim_client_t client(contract_addr);
  auto auth_claims = make_auth_claims();

  {
    SignedTransaction stx;
    stx.tx.invocation =
        client.make_invocation<"test_invoked_address_signed_weight">(
            UINT64_MAX);
    stx.tx.auth_claims = auth_claims;

    auto handler = make_new_system_call_handler(stx, *context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

TEST_F(AuthClaimTestFixture, TestOtherAddressSignature)
{
  auto contract = hogd::load_contract("authclaim_test.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  authclaim_client_t client(contract_addr);
  auto auth_claims = make_auth_claims();

  {
    SignedTransaction stx;
    stx.tx.invocation =
        client.make_invocation<"test_other_address_signature">(UINT64_MAX);
    stx.tx.auth_claims = auth_claims;

    auto handler = make_new_system_call_handler(stx, *context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

TEST_F(AuthClaimTestFixture, TestOtherAddressSignedWeight)
{
  auto contract = hogd::load_contract("authclaim_test.wasm");
  auto contract_addr = make_contract_address(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  authclaim_client_t client(contract_addr);
  auto auth_claims = make_auth_claims();

  {
    SignedTransaction stx;
    stx.tx.invocation =
        client.make_invocation<"test_other_address_signed_weight">(UINT64_MAX);
    stx.tx.auth_claims = auth_claims;

    auto handler = make_new_system_call_handler(stx, *context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

} // namespace hogd
