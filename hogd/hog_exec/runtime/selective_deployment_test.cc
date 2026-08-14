#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/wasm/test_load_wasm.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class SelectiveDeploymentTest : public ::testing::TestWithParam<TestDB>,
                                public NativeContractTestFramework {
public:
  SelectiveDeploymentTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

namespace {

// Test-only stub that accepts any wasmi contract without parsing it. Tests in
// this file exercise SystemCallHandler's selective-deployment bookkeeping
// (which contracts end up in the DB), not wasm validation, and use opaque
// placeholder bytes in contracts_to_deploy rather than real wasm modules. A
// real wasmi context would reject those bytes in verify_contract and the test
// would fail for unrelated reasons. The deployed contracts are never invoked,
// so make_new_runtime does not need to produce a working Runtime.
class NoopWasmiContext : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const & /*unused*/) const override
  {
    return InvokeErrorCode::None;
  }

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const & /*unused*/,
                   SystemCallHandler * /*unused*/) override
  {
    return nullptr;
  }
};

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Test that verifies when a transaction has two contracts in
// contracts_to_deploy, but the invoked contract only deploys the first one,
// then after commit:
// - The first contract is in the database
// - The second contract is NOT in the database
TEST_P(SelectiveDeploymentTest, DeployOnlyFirstContract)
{
  // Create two contracts to add to contracts_to_deploy
  Contract contract_to_deploy_1(ContractType::kWasmi);
  contract_to_deploy_1.wasmi_contract() =
      ContractBlob{.data = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1}};

  Contract contract_to_deploy_2(ContractType::kWasmi);
  contract_to_deploy_2.wasmi_contract() =
      ContractBlob{.data = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}};

  auto contract_1_hash = sha256(contract_to_deploy_1);
  auto contract_2_hash = sha256(contract_to_deploy_2);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 1;
    return nonce;
  }();

  // Create a native contract that will deploy only the first contract
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(0,
                [contract_1_hash](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  // Deploy only the first contract

                  auto deploy_result =
                      ctx.handler().deploy_contract(kDeployNonce,
                                                    contract_1_hash);
                  if (!deploy_result.has_value()) {
                    return deploy_result.error().to_invoke_error();
                  }
                  if (!deploy_result.value().has_value()) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }

                  return {};
                });
  auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  NativeContext native_context(std::move(contract_map));
  NoopWasmiContext wasmi_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kWasmi, {&wasmi_context, false}}});

  // Mark the deployer contract as existing and deployed within the database
  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  // To verify the deployment addresses
  auto deployed_addr_1 =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   contract_1_hash);

  // Grant storage tokens to the deployer and the deployed contract
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr_1, 1'000'000'000);

  // Setup the transaction with both contracts in contracts_to_deploy
  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
          .contracts_to_deploy = {contract_to_deploy_1, contract_to_deploy_2},
      }};
  auto handler = make_new_system_call_handler(handler_tx, context);

  // Invoke the contract
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // Verify that contract_1 is in the database (it was deployed)
  auto contract_1_lookup = lookup_contract_from_db(contract_1_hash);
  EXPECT_TRUE(contract_1_lookup.has_value())
      << "First contract should be in database after deployment";

  // Verify that contract_2 is NOT in the database (it was not deployed)
  auto contract_2_lookup = lookup_contract_from_db(contract_2_hash);
  EXPECT_FALSE(contract_2_lookup.has_value())
      << "Second contract should NOT be in database (was not deployed)";

  // Check that the first contract is deployed at the expected address
  auto deployment_1 = lookup_deployment_from_db(deployed_addr_1);
  ASSERT_TRUE(deployment_1.has_value());
  EXPECT_EQ(deployment_1.value().hash, contract_1_hash);
}

// Test that verifies attempting to deploy a precompiled (native) contract fails
// with kSyscallErrorCodeInvalidSyscallInput. Native contracts like the HOG
// token are set up at genesis and should not be redeployable.
TEST_P(SelectiveDeploymentTest, CannotDeployPrecompiledNativeContract)
{
  // Create a native contract (the precompiled token contract)
  auto native_token_contract = make_native_contract(0);
  auto native_token_hash = sha256(*native_token_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 42;
    return nonce;
  }();

  // Create a native contract that will attempt to deploy the native token
  auto make_context = [native_token_hash]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
    fns_0.emplace(
        0,
        [native_token_hash](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          // Attempt to deploy the native token contract - should fail
          ContractAddress deploy_addr;
          auto syscall_res =
              ctx.syscall<SyscallNo::kDeployContract>(kDeployNonce,
                                                      native_token_hash,
                                                      deploy_addr);
          if (!syscall_res.has_value() ||
              syscall_res.value() !=
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  // Mark the deployer contract as existing and deployed within the database
  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  // Write the native token contract to the database (it exists but cannot be
  // deployed)
  write_contract(native_token_contract);

  // Grant storage tokens to the deployer
  setup_grant_storage_token(deployer_addr, 1'000'000'000);

  // Setup the transaction - note we don't need the native token contract in
  // contracts_to_deploy since it already exists in the database
  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context);

  // Invoke the contract - it should succeed (contract caught the error)
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  ContractAddress deploy_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   native_token_hash);

  // Verify that the deployment failed
  auto deployment_lookup = lookup_deployment_from_db(deploy_addr);
  EXPECT_FALSE(deployment_lookup.has_value());
}

// Regression test for issue #2021: a transaction that attaches a wasm contract
// with unknown host imports must be rejected at initial validation (before
// root_invoke executes), so the malformed contract never reaches deployment.
TEST_P(SelectiveDeploymentTest, MalformedImportAbiRejectedBeforeExecution)
{
  Contract malformed_contract(ContractType::kWasmi);
  malformed_contract.wasmi_contract() =
      test::load_wasm_from_file("test_unknown_import.wasm");
  auto malformed_hash = sha256(malformed_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 7;
    return nonce;
  }();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_deployer;
  fns_deployer.emplace(0,
                       [malformed_hash](NativeSyscallContext &ctx) noexcept
                           -> InvokeStatus<void> {
                         auto deploy_result =
                             ctx.handler().deploy_contract(kDeployNonce,
                                                           malformed_hash);
                         if (!deploy_result.has_value()) {
                           return deploy_result.error().to_invoke_error();
                         }
                         if (!deploy_result.value().has_value()) {
                           return InvokeError{InvokeErrorCode::Deterministic};
                         }
                         return {};
                       });
  auto native_deployer =
      std::make_unique<NativeContract>(std::move(fns_deployer));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_deployer));
  NativeContext native_context(std::move(contract_map));
  auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kWasmi, {wasm_context.get(), false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   malformed_hash);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr, 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
          .contracts_to_deploy = {malformed_contract},
      }};
  auto handler = make_new_system_call_handler(handler_tx, context);

  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success());
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Deterministic});
}

// Regression test for issue #2063: when a malformed attached contract causes
// initial validation to record a deterministic error before root execution,
// unused nondeterministic proposal data must still be detected and the
// transaction classified as ExecResultTxValidationFailure.
TEST_P(SelectiveDeploymentTest,
       InitialValidationContractFailureChecksUnusedProposalBits)
{
  Contract malformed_contract(ContractType::kWasmi);
  malformed_contract.wasmi_contract() =
      test::load_wasm_from_file("test_unknown_import.wasm");
  auto malformed_hash = sha256(malformed_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 8;
    return nonce;
  }();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_deployer;
  fns_deployer.emplace(0,
                       [malformed_hash](NativeSyscallContext &ctx) noexcept
                           -> InvokeStatus<void> {
                         auto deploy_result =
                             ctx.handler().deploy_contract(kDeployNonce,
                                                           malformed_hash);
                         if (!deploy_result.has_value()) {
                           return deploy_result.error().to_invoke_error();
                         }
                         if (!deploy_result.value().has_value()) {
                           return InvokeError{InvokeErrorCode::Deterministic};
                         }
                         return {};
                       });
  auto native_deployer =
      std::make_unique<NativeContract>(std::move(fns_deployer));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_deployer));
  NativeContext native_context(std::move(contract_map));
  auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kWasmi, {wasm_context.get(), false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   malformed_hash);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr, 1'000'000'000);

  Transaction tx{
      .invocation = Invocation{.invoked_address = deployer_addr,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
      .uniquifier = make_new_uniquifier(),
      .contracts_to_deploy = {malformed_contract},
  };

  NondeterministicProposalData proposal_data;
  proposal_data.nondeterministic_bits = {uint8_t{1}};
  proposal_data.nondeterministic_bit_count = 1;

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, context,
                                              std::move(proposal_data));
  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success());

  EXPECT_EQ(handler.try_commit_transaction(),
            ExecResult::ExecResultTxValidationFailure)
      << "Unused proposal bit must cause validation failure even when initial "
         "validation rejects a malformed attached contract";
}

// A contract whose serialized code row exceeds one RAMDB item deploys
// end-to-end: hog_db stores the row chunked, transparently to hog_exec
// (#2569). 100 KiB is between the ~64 KiB single-item ceiling and
// kMaxContractSizeBytes.
TEST_P(SelectiveDeploymentTest, DeploysContractLargerThanOneStorageItem)
{
  Contract big_contract(ContractType::kWasmi);
  big_contract.wasmi_contract().data.resize_uninitialized(100_KiB);
  for (size_t i = 0; i < big_contract.wasmi_contract().data.size(); ++i) {
    big_contract.wasmi_contract().data[i] = static_cast<uint8_t>((i * 31) + 7);
  }
  auto big_hash = sha256(big_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 99;
    return nonce;
  }();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(0,
                [big_hash](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto deploy_result =
                      ctx.handler().deploy_contract(kDeployNonce, big_hash);
                  if (!deploy_result.has_value()) {
                    return deploy_result.error().to_invoke_error();
                  }
                  if (!deploy_result.value().has_value()) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
  auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  NativeContext native_context(std::move(contract_map));
  NoopWasmiContext wasmi_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kWasmi, {&wasmi_context, false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   big_hash);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr, 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
          .contracts_to_deploy = {big_contract},
      }};
  auto handler = make_new_system_call_handler(handler_tx, context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();

  auto lookup = lookup_contract_from_db(big_hash);
  ASSERT_TRUE(lookup.has_value());
  EXPECT_EQ(sha256(lookup.value()), big_hash);
}

// An attached contract blob above kMaxContractSizeBytes is rejected
// deterministically during initial validation: the body never runs and
// nothing is deployed (#2569).
TEST_P(SelectiveDeploymentTest, RejectsAttachedContractAboveSizeLimit)
{
  Contract oversized(ContractType::kWasmi);
  oversized.wasmi_contract().data.resize_uninitialized(kMaxContractSizeBytes +
                                                       1);
  auto oversized_hash = sha256(oversized);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "transaction body must not execute when an "
                         "attached contract is oversized";
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  NativeContext native_context(std::move(contract_map));
  NoopWasmiContext wasmi_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kWasmi, {&wasmi_context, false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  setup_grant_storage_token(deployer_addr, 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
          .contracts_to_deploy = {oversized},
      }};
  auto handler = make_new_system_call_handler(handler_tx, context);

  auto res = handler.root_invoke();
  EXPECT_FALSE(res.is_root_success());
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Deterministic});
  EXPECT_EQ(handler.try_commit_transaction(),
            ExecResult::ExecResultTxDeterministicError);
  advance_block();
  EXPECT_FALSE(lookup_contract_from_db(oversized_hash).has_value());
}

// A single transaction can resolve at most kMaxDistinctContractResolutionsPerTx
// distinct deployed contracts. The first resolution of a distinct callee warms
// a full contract entry into the block-scoped snapshot cache and retains it for
// the rest of the block, and resolution happens before the child frame runs or
// spends gas -- so without this bound a contract could invoke many distinct
// already-deployed callees (even with gas_limit 0 / TryInvoke) and pin retained
// code out of proportion to the gas it spends (#3375). The root's own
// resolution counts against the cap, so it can resolve
// kMaxDistinctContractResolutionsPerTx - 1 further distinct callees; the next
// distinct invoke is rejected with kSyscallErrorCodeInvalidSyscallInput before
// the callee is fetched, while re-invoking an already-resolved callee still
// succeeds (it retains nothing new).
TEST_P(SelectiveDeploymentTest, CapsDistinctContractResolutionsPerTransaction)
{
  constexpr size_t kExtraChildren = 3;
  constexpr size_t kChildCount =
      kMaxDistinctContractResolutionsPerTx + kExtraChildren;
  // The root's own resolution consumes one slot.
  constexpr size_t kAllowedChildren = kMaxDistinctContractResolutionsPerTx - 1;

  // One tiny native callee, deployed at many distinct addresses. The snapshot
  // cache dedups the identical code by hash, but each distinct address is a
  // distinct resolution against the per-tx cap -- and distinct retained hashes
  // are at most the number of distinct resolved addresses, so bounding the
  // latter bounds retained code.
  auto make_child_addr = [](size_t i) {
    ContractAddress addr{};
    addr[0] = static_cast<uint8_t>(i & 0xFF);
    addr[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
    // Tag byte keeps children clear of the single-byte root address below.
    addr[2] = 0xC0;
    return addr;
  };

  std::vector<ContractAddress> child_addresses;
  child_addresses.reserve(kChildCount);
  for (size_t i = 0; i < kChildCount; ++i) {
    child_addresses.push_back(make_child_addr(i));
  }

  // Results of invoking each distinct child in order, plus a trailing re-invoke
  // of the first (already-resolved) child. Recorded by the root method.
  std::vector<SyscallErrorCode> results;
  results.reserve(kChildCount);
  std::optional<SyscallErrorCode> reinvoke_result;

  auto invoke_child = [](NativeSyscallContext &ctx,
                         ContractAddress const &addr) {
    CopyableInvokeInputs inputs{
        .entrypoint_id = 0,
        .gas_limit = UINT64_MAX,
        .flags = std::to_underlying(InvokeFlag::TryInvoke),
    };
    CopyableInvokeOutputs outputs{};
    return ctx.syscall<SyscallNo::kInvoke>(addr, std::span<const std::byte>{},
                                           std::span<const std::byte>{}, inputs,
                                           outputs);
  };

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> child_fns;
  child_fns.emplace(0,
                    [](NativeSyscallContext & /*ctx*/) noexcept
                        -> InvokeStatus<void> { return {}; });

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> root_fns;
  root_fns.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        for (auto const &addr : child_addresses) {
          auto r = invoke_child(ctx, addr);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          results.push_back(r.value());
        }
        auto r = invoke_child(ctx, child_addresses.front());
        if (!r.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        reinvoke_result = r.value();
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
  native_map.emplace(0, std::make_unique<NativeContract>(std::move(root_fns)));
  native_map.emplace(1, std::make_unique<NativeContract>(std::move(child_fns)));
  NativeContext native_context(std::move(native_map));

  auto root_contract = make_native_contract(0);
  write_contract(root_contract);
  auto root_addr = make_contract_address(200);
  write_deployment(root_addr, root_contract);

  auto child_contract = make_native_contract(1);
  write_contract(child_contract);
  for (auto const &addr : child_addresses) {
    write_deployment(addr, child_contract);
  }

  Transaction tx{
      .invocation = Invocation{.invoked_address = root_addr,
                               .entrypoint = entrypoint_from_method_id(0),
                               .gas_limit = UINT64_MAX,
                               .calldata = {}},
      .uniquifier = make_new_uniquifier(),
  };
  auto handler = make_new_system_call_handler(tx, native_context);
  auto result = handler.root_invoke();
  ASSERT_TRUE(result.is_root_success());

  ASSERT_EQ(results.size(), kChildCount);
  for (size_t i = 0; i < kChildCount; ++i) {
    if (i < kAllowedChildren) {
      EXPECT_EQ(results[i], SyscallErrorCode::kSyscallErrorCodeNone)
          << "distinct callee " << i << " within the per-tx cap should resolve";
    }
    else {
      EXPECT_EQ(results[i],
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput)
          << "distinct callee " << i << " beyond the per-tx cap is rejected";
    }
  }
  ASSERT_TRUE(reinvoke_result.has_value());
  EXPECT_EQ(*reinvoke_result, SyscallErrorCode::kSyscallErrorCodeNone)
      << "re-invoking an already-resolved callee is not a new resolution";
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, SelectiveDeploymentTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
