#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class SelectiveDeploymentTest : public ::testing::TestWithParam<TestDB>,
                                public NativeContractTestFramework {
public:
  SelectiveDeploymentTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

namespace {

// Test-only stub that accepts any rv32-tagged contract without parsing it.
// Tests in this file exercise SystemCallHandler's selective-deployment
// bookkeeping (which contracts end up in the DB), not artifact validation, and
// use opaque placeholder bytes in contracts_to_deploy rather than real
// artifacts. A real Rv32Context would reject those bytes in verify_contract
// and the test would fail for unrelated reasons. The deployed contracts are
// never invoked, so make_new_runtime does not need to produce a working
// Runtime.
class NoopRv32Context : public RuntimeContext {
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
  Contract contract_to_deploy_1(ContractType::kRv32);
  contract_to_deploy_1.rv32_contract() =
      ContractBlob{.data = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1}};

  Contract contract_to_deploy_2(ContractType::kRv32);
  contract_to_deploy_2.rv32_contract() =
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
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

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

namespace {

// An rv32 blob that fails Rv32Context::verify_contract (not a valid artifact).
Contract
make_untranslatable_rv32_contract()
{
  Contract contract(ContractType::kRv32);
  contract.rv32_contract().data.append_range(
      std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});
  return contract;
}

} // namespace

// rv32 twin of MalformedImportAbiRejectedBeforeExecution: a transaction that
// attaches an untranslatable rv32 contract must be rejected at initial
// validation, before root_invoke executes, so nothing reaches deployment.
TEST_P(SelectiveDeploymentTest, MalformedRv32RejectedBeforeExecution)
{
  Contract malformed_contract = make_untranslatable_rv32_contract();
  auto malformed_hash = sha256(malformed_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 9;
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
  Rv32Context rv32_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&rv32_context, false}}});

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

  auto deployment_lookup = lookup_deployment_from_db(deployed_addr);
  EXPECT_FALSE(deployment_lookup.has_value());
}

// rv32 twin of InitialValidationContractFailureChecksUnusedProposalBits: the
// early reject of an untranslatable attached rv32 contract must still run the
// unused-proposal-bit check in validation mode.
TEST_P(SelectiveDeploymentTest,
       InitialValidationRv32FailureChecksUnusedProposalBits)
{
  Contract malformed_contract = make_untranslatable_rv32_contract();
  auto malformed_hash = sha256(malformed_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 10;
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
  Rv32Context rv32_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&rv32_context, false}}});

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
         "validation rejects a malformed attached rv32 contract";
}

// A contract deployed via the deploy syscall is invokable in the same
// transaction: the attachment is translated once during initial validation and
// cached in local_rv32_translations_, so both in-transaction invokes reuse that
// translation rather than lowering the bytes again.
TEST_P(SelectiveDeploymentTest, DeployedRv32ContractInvokableInSameTransaction)
{
  Contract fib_contract(ContractType::kRv32);
  fib_contract.rv32_contract() = load_contract_blob("fibonacci.rv32a");
  auto fib_hash = sha256(fib_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 11;
    return nonce;
  }();

  auto deployer_addr = make_contract_address(1);
  auto deploy_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   fib_hash);
  ContractTraitClient<fibonacci::Fibonacci> fib_client(deploy_addr);
  const Invocation inner =
      fib_client.make_invocation<u8"fibonacci">(1'000'000, 5U);
  const uint64_t entry_id = sdk_method_id(inner.entrypoint);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_deployer;
  fns_deployer.emplace(
      0,
      [fib_hash, entry_id, calldata = inner.calldata](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto deploy_result =
            ctx.handler().deploy_contract(kDeployNonce, fib_hash);
        if (!deploy_result.has_value()) {
          ADD_FAILURE() << "deploy_contract system error";
          return deploy_result.error().to_invoke_error();
        }
        if (!deploy_result.value().has_value()) {
          ADD_FAILURE() << "deploy_contract syscall error: "
                        << deploy_result.value().error().error_code().message();
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        const ContractAddress target = deploy_result.value().value();
        for (int i = 0; i < 2; ++i) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = entry_id,
              .gas_limit = 1'000'000,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          CopyableInvokeOutputs outputs{};
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              target,
              std::span<const std::byte>(reinterpret_cast<const std::byte *>(
                                             calldata.data()),
                                         calldata.size()),
              std::span<uint32_t>{}, inputs, outputs);
          if (!r.has_value()) {
            ADD_FAILURE() << "invoke " << i << " host error: "
                          << r.error().error_code().message();
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "invoke " << i << " syscall error "
                          << std::to_underlying(*r);
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (outputs.frame_status != InvocationFrameStatusCode::Return ||
              outputs.return_buffer_len == 0) {
            ADD_FAILURE() << "invoke " << i << " frame status "
                          << std::to_underlying(outputs.frame_status)
                          << " return len " << outputs.return_buffer_len;
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          std::array<std::byte, 8> buffer{};
          uint32_t written_len = 0;
          std::array<std::byte, 4> token_buffer{};
          uint32_t token_written_len = 0;
          auto fetched = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
              std::span<std::byte>(buffer), written_len,
              std::span<std::byte>(token_buffer), token_written_len);
          if (!fetched.has_value() ||
              *fetched != SyscallErrorCode::kSyscallErrorCodeNone ||
              written_len != outputs.return_buffer_len) {
            ADD_FAILURE() << "invoke " << i << " returned-buffer fetch failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          // fibonacci(5) == 5, CXS-encoded in a single byte.
          if (written_len != 1 || buffer[0] != std::byte{5}) {
            ADD_FAILURE() << "invoke " << i << " unexpected fibonacci result";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      });
  auto native_deployer =
      std::make_unique<NativeContract>(std::move(fns_deployer));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_deployer));
  NativeContext native_context(std::move(contract_map));
  Rv32Context rv32_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&rv32_context, false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  write_deployment(deployer_addr, deployer_contract);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deploy_addr, 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
          .contracts_to_deploy = {fib_contract},
      }};
  auto handler = make_new_system_call_handler(handler_tx, context);

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  advance_block();

  auto deployment_lookup = lookup_deployment_from_db(deploy_addr);
  EXPECT_TRUE(deployment_lookup.has_value());
}

// A deployed contract whose type has no registered runtime context should be
// impossible (verify_contract rejects unknown types before deployment), so
// invoking one is Unrecoverable, consuming no gas.
TEST_P(SelectiveDeploymentTest, InvokeOfUnregisteredContractTypeIsUnrecoverable)
{
  auto contract = std::make_shared<Contract>(ContractType::kMockTest);
  contract->mock_contract_id() = 42;
  write_contract(contract);
  auto addr = make_contract_address(1);
  write_deployment(addr, contract);
  setup_grant_storage_token(addr, 1'000'000'000);

  NativeContext native_context({});
  Rv32Context rv32_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&rv32_context, false}}});

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = addr,
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = 1'000'000,
                                    .calldata = {},
                                },
                            .uniquifier = make_new_uniquifier()}},
      context);

  auto res = handler.root_invoke();
  ASSERT_FALSE(res.is_root_success());
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Unrecoverable});
  EXPECT_EQ(res.gas_consumed_, 0U);
}

// A contract whose serialized code row exceeds one RAMDB item deploys
// end-to-end: hog_db stores the row chunked, transparently to hog_exec
// (#2569). 100 KiB is between the ~64 KiB single-item ceiling and
// kMaxContractSizeBytes.
TEST_P(SelectiveDeploymentTest, DeploysContractLargerThanOneStorageItem)
{
  Contract big_contract(ContractType::kRv32);
  big_contract.rv32_contract().data.resize_uninitialized(100_KiB);
  for (size_t i = 0; i < big_contract.rv32_contract().data.size(); ++i) {
    big_contract.rv32_contract().data[i] = static_cast<uint8_t>((i * 31) + 7);
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
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

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

// Redeploying the same (sender, nonce, contract_hash) in a LATER BLOCK is
// rejected via the snapshot lookup with the exact code
// kSyscallErrorCodeContractAlreadyDeployed. The existing pin (guest
// test_deploy_contract_already_deployed) deploys twice in one transaction and
// only exercises the tx-local lookup; dropping the snapshot check would pass
// it while letting a cross-block redeploy re-stage over a committed
// deployment.
TEST_P(SelectiveDeploymentTest, CrossBlockRedeployRejectedAtOccupiedAddress)
{
  Contract blob(ContractType::kRv32);
  blob.rv32_contract().data.resize_uninitialized(64);
  for (size_t i = 0; i < blob.rv32_contract().data.size(); ++i) {
    blob.rv32_contract().data[i] = static_cast<uint8_t>((i * 3) + 1);
  }
  auto blob_hash = sha256(blob);

  constexpr static SHA256Hash kNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 42;
    return nonce;
  }();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [blob_hash](
                  NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                auto res = ctx.handler().deploy_contract(kNonce, blob_hash);
                if (!res.has_value()) {
                  return res.error().to_invoke_error();
                }
                if (!res.value().has_value()) {
                  ADD_FAILURE() << "first deploy must succeed";
                  return InvokeError{InvokeErrorCode::Deterministic};
                }
                return {};
              });
  fns.emplace(
      1, [blob_hash](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto res = ctx.handler().deploy_contract(kNonce, blob_hash);
        if (!res.has_value()) {
          return res.error().to_invoke_error();
        }
        if (res.value().has_value() ||
            res.value().error().code() !=
                SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed) {
          ADD_FAILURE() << "cross-block redeploy must be "
                           "kSyscallErrorCodeContractAlreadyDeployed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  NativeContext native_context(std::move(contract_map));
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kNonce,
                                                   blob_hash);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr, 1'000'000'000);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier(),
                    .contracts_to_deploy = {blob},
                }},
        context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  advance_block();

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier(),
                }},
        context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  advance_block();
  auto row = lookup_deployment_from_db(deployed_addr);
  ASSERT_TRUE(row.has_value());
}

// Deployments staged inside a child frame roll back when the frame manually
// reverts or faults: commit_manually_reverted_frame / commit_failed_child_frame
// drop the child's new_contract_deployments_. The probe is a same-tx redeploy
// of the identical (sender, nonce, hash) from a fresh child frame -- it only
// succeeds if the earlier deployments were really rolled back.
TEST_P(SelectiveDeploymentTest, ChildFrameFailureRollsBackDeployment)
{
  Contract blob(ContractType::kRv32);
  blob.rv32_contract().data.resize_uninitialized(64);
  for (size_t i = 0; i < blob.rv32_contract().data.size(); ++i) {
    blob.rv32_contract().data[i] = static_cast<uint8_t>((i * 5) + 3);
  }
  auto blob_hash = sha256(blob);

  constexpr static SHA256Hash kNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 43;
    return nonce;
  }();

  auto parent_addr = make_contract_address(1);
  auto child_addr = make_contract_address(2);
  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(child_addr, kNonce,
                                                   blob_hash);

  // Child: deploy, assert the expected address, then end the frame as
  // directed -- method 0 manually reverts, method 1 faults, method 2 (the
  // probe) also reverts so the test leaves no deployment behind.
  auto deploy_then =
      [blob_hash, deployed_addr](NativeSyscallContext &ctx,
                                 bool fault) noexcept -> InvokeStatus<void> {
    auto res = ctx.handler().deploy_contract(kNonce, blob_hash);
    if (!res.has_value()) {
      return res.error().to_invoke_error();
    }
    if (!res.value().has_value()) {
      ADD_FAILURE() << "child deploy must succeed (rollback leaked?): code "
                    << res.value().error().raw_code();
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (res.value().value() != deployed_addr) {
      ADD_FAILURE() << "deploy landed at an unexpected address";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (fault) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    ctx.handler().set_return_buffer(ReturnBuffer{}, /*revert_frame=*/true);
    return {};
  };

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(0, [deploy_then](NativeSyscallContext &ctx) noexcept {
    return deploy_then(ctx, /*fault=*/false);
  });
  fns_child.emplace(1, [deploy_then](NativeSyscallContext &ctx) noexcept {
    return deploy_then(ctx, /*fault=*/true);
  });
  fns_child.emplace(2, [deploy_then](NativeSyscallContext &ctx) noexcept {
    return deploy_then(ctx, /*fault=*/false);
  });

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [child_addr](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto invoke_child =
            [&](Runtime::MethodID method,
                InvocationFrameStatusCode expected) -> InvokeStatus<void> {
          CopyableInvokeOutputs outputs{};
          CopyableInvokeInputs inputs{.entrypoint_id = method,
                                      .gas_limit = UINT64_MAX,
                                      .flags = 0};
          auto r = ctx.syscall<SyscallNo::kInvoke>(child_addr,
                                                   std::span<const std::byte>{},
                                                   std::span<uint32_t>{},
                                                   inputs, outputs);
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "invoke of child method " << method << " failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (outputs.frame_status != expected) {
            ADD_FAILURE() << "child method " << method
                          << " ended with unexpected frame status";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        };

        // Reverted child: deployment must roll back.
        if (auto r = invoke_child(0, InvocationFrameStatusCode::Abort); !r) {
          return r;
        }
        // Faulted child: deployment must roll back.
        if (auto r = invoke_child(1, InvocationFrameStatusCode::Fault); !r) {
          return r;
        }
        // Probe: the same (nonce, hash) deploys again only if both earlier
        // deployments were rolled back.
        return invoke_child(2, InvocationFrameStatusCode::Abort);
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0,
                       std::make_unique<NativeContract>(std::move(fns_parent)));
  contract_map.emplace(1,
                       std::make_unique<NativeContract>(std::move(fns_child)));
  NativeContext native_context(std::move(contract_map));
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto parent_contract = make_native_contract(0);
  write_contract(parent_contract);
  write_deployment(parent_addr, parent_contract);
  auto child_contract = make_native_contract(1);
  write_contract(child_contract);
  write_deployment(child_addr, child_contract);

  setup_grant_storage_token(parent_addr, 1'000'000'000);
  setup_grant_storage_token(child_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = parent_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
                  .contracts_to_deploy = {blob},
              }},
      context);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);

  advance_block();
  EXPECT_FALSE(lookup_deployment_from_db(deployed_addr).has_value());
}

// Deployments staged inside a TryInvoke child follow the enclosing frames'
// outcomes: they commit with the transaction when every enclosing frame
// succeeds, and are popped by mark_frame_as_failed when a LATER fault of the
// try-invoking frame is caught by its parent (the root still commits).
TEST_P(SelectiveDeploymentTest, TryInvokeDeploymentFollowsFrameOutcome)
{
  Contract blob(ContractType::kRv32);
  blob.rv32_contract().data.resize_uninitialized(64);
  for (size_t i = 0; i < blob.rv32_contract().data.size(); ++i) {
    blob.rv32_contract().data[i] = static_cast<uint8_t>((i * 7) + 5);
  }
  auto blob_hash = sha256(blob);

  constexpr static SHA256Hash kNonceKept = []() {
    SHA256Hash nonce{};
    nonce[0] = 44;
    return nonce;
  }();
  constexpr static SHA256Hash kNonceDropped = []() {
    SHA256Hash nonce{};
    nonce[0] = 45;
    return nonce;
  }();

  auto root_addr = make_contract_address(1);
  auto mid_addr = make_contract_address(2);
  auto leaf_addr = make_contract_address(3);
  auto kept_addr =
      SystemCallHandler::static_get_deploy_address(leaf_addr, kNonceKept,
                                                   blob_hash);
  auto dropped_addr =
      SystemCallHandler::static_get_deploy_address(leaf_addr, kNonceDropped,
                                                   blob_hash);

  // Leaf: deploy under the method-selected nonce and return successfully.
  auto leaf_deploy =
      [blob_hash](NativeSyscallContext &ctx,
                  SHA256Hash const &nonce) noexcept -> InvokeStatus<void> {
    auto res = ctx.handler().deploy_contract(nonce, blob_hash);
    if (!res.has_value()) {
      return res.error().to_invoke_error();
    }
    if (!res.value().has_value()) {
      ADD_FAILURE() << "leaf deploy must succeed";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  };
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_leaf;
  fns_leaf.emplace(0, [leaf_deploy](NativeSyscallContext &ctx) noexcept {
    return leaf_deploy(ctx, kNonceKept);
  });
  fns_leaf.emplace(1, [leaf_deploy](NativeSyscallContext &ctx) noexcept {
    return leaf_deploy(ctx, kNonceDropped);
  });

  // Mid: try-invoke the leaf (which succeeds), then either fault (method 0)
  // or succeed (method 1).
  auto mid_body = [leaf_addr](NativeSyscallContext &ctx,
                              Runtime::MethodID leaf_method,
                              bool fault_after) noexcept -> InvokeStatus<void> {
    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{.entrypoint_id = leaf_method,
                                .gas_limit = UINT64_MAX,
                                .flags =
                                    std::to_underlying(InvokeFlag::TryInvoke)};
    auto r =
        ctx.syscall<SyscallNo::kInvoke>(leaf_addr, std::span<const std::byte>{},
                                        std::span<uint32_t>{}, inputs, outputs);
    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
        outputs.frame_status != InvocationFrameStatusCode::Return) {
      ADD_FAILURE() << "try-invoked leaf must return successfully";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (fault_after) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  };
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_mid;
  fns_mid.emplace(0, [mid_body](NativeSyscallContext &ctx) noexcept {
    return mid_body(ctx, 1, /*fault_after=*/true);
  });
  fns_mid.emplace(1, [mid_body](NativeSyscallContext &ctx) noexcept {
    return mid_body(ctx, 0, /*fault_after=*/false);
  });

  // Root: invoke mid and expect the method-dependent frame outcome.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_root;
  auto root_body = [mid_addr](NativeSyscallContext &ctx,
                              Runtime::MethodID mid_method,
                              InvocationFrameStatusCode expected) noexcept
      -> InvokeStatus<void> {
    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{.entrypoint_id = mid_method,
                                .gas_limit = UINT64_MAX,
                                .flags = 0};
    auto r =
        ctx.syscall<SyscallNo::kInvoke>(mid_addr, std::span<const std::byte>{},
                                        std::span<uint32_t>{}, inputs, outputs);
    if (!r.has_value() ||
        r.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
        outputs.frame_status != expected) {
      ADD_FAILURE() << "mid frame ended with unexpected status";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  };
  fns_root.emplace(0, [root_body](NativeSyscallContext &ctx) noexcept {
    return root_body(ctx, 0, InvocationFrameStatusCode::Fault);
  });
  fns_root.emplace(1, [root_body](NativeSyscallContext &ctx) noexcept {
    return root_body(ctx, 1, InvocationFrameStatusCode::Return);
  });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0,
                       std::make_unique<NativeContract>(std::move(fns_root)));
  contract_map.emplace(1, std::make_unique<NativeContract>(std::move(fns_mid)));
  contract_map.emplace(2,
                       std::make_unique<NativeContract>(std::move(fns_leaf)));
  NativeContext native_context(std::move(contract_map));
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto root_contract = make_native_contract(0);
  write_contract(root_contract);
  write_deployment(root_addr, root_contract);
  auto mid_contract = make_native_contract(1);
  write_contract(mid_contract);
  write_deployment(mid_addr, mid_contract);
  auto leaf_contract = make_native_contract(2);
  write_contract(leaf_contract);
  write_deployment(leaf_addr, leaf_contract);

  setup_grant_storage_token(root_addr, 1'000'000'000);
  setup_grant_storage_token(mid_addr, 1'000'000'000);
  setup_grant_storage_token(leaf_addr, 1'000'000'000);
  setup_grant_storage_token(kept_addr, 1'000'000'000);
  setup_grant_storage_token(dropped_addr, 1'000'000'000);

  auto run_root = [&](Runtime::MethodID method) {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = root_addr,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier(),
                    .contracts_to_deploy = {blob},
                }},
        context);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  };

  // Mid faults after the successful TryInvoke: the leaf's deployment is
  // popped with the failed frame even though the root commits.
  run_root(0);
  // Every frame succeeds: the leaf's deployment commits.
  run_root(1);

  advance_block();
  EXPECT_FALSE(lookup_deployment_from_db(dropped_addr).has_value());
  auto kept = lookup_deployment_from_db(kept_addr);
  ASSERT_TRUE(kept.has_value());
}

// GetDeployAddress arm selection and output values: the null caller arm uses
// the calling frame's own address, the explicit arm uses the supplied
// address, and the null-arm result is exactly where DeployContract lands.
TEST_P(SelectiveDeploymentTest, GetDeployAddressArmsMatchActualDeployment)
{
  Contract blob(ContractType::kRv32);
  blob.rv32_contract().data.resize_uninitialized(64);
  for (size_t i = 0; i < blob.rv32_contract().data.size(); ++i) {
    blob.rv32_contract().data[i] = static_cast<uint8_t>((i * 11) + 7);
  }
  auto blob_hash = sha256(blob);

  constexpr static SHA256Hash kNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 46;
    return nonce;
  }();

  auto deployer_addr = make_contract_address(1);
  auto other_addr = make_contract_address(9);
  auto expected_self =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kNonce,
                                                   blob_hash);
  auto expected_other =
      SystemCallHandler::static_get_deploy_address(other_addr, kNonce,
                                                   blob_hash);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [blob_hash, other_addr, expected_self, expected_other](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        ContractAddress out_null{};
        auto s1 = ctx.syscall<SyscallNo::kGetDeployAddress>(
            static_cast<const ContractAddress *>(nullptr), kNonce, blob_hash,
            out_null);
        if (!s1.has_value() ||
            s1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            out_null != expected_self) {
          ADD_FAILURE() << "null-caller arm must derive from the current "
                           "frame's address";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        ContractAddress out_explicit{};
        auto s2 =
            ctx.syscall<SyscallNo::kGetDeployAddress>(&other_addr, kNonce,
                                                      blob_hash, out_explicit);
        if (!s2.has_value() ||
            s2.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            out_explicit != expected_other || out_explicit == out_null) {
          ADD_FAILURE() << "explicit-caller arm must derive from the "
                           "supplied address";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto deploy = ctx.handler().deploy_contract(kNonce, blob_hash);
        if (!deploy.has_value()) {
          return deploy.error().to_invoke_error();
        }
        if (!deploy.value().has_value() || deploy.value().value() != out_null) {
          ADD_FAILURE() << "DeployContract must land at the null-arm address";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  NativeContext native_context(std::move(contract_map));
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  write_deployment(deployer_addr, deployer_contract);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(expected_self, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = deployer_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
                  .contracts_to_deploy = {blob},
              }},
      context);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);

  advance_block();
  auto row = lookup_deployment_from_db(expected_self);
  ASSERT_TRUE(row.has_value());
}

// The attached-contract size gate is strict (> kMaxContractSizeBytes): a
// blob of exactly kMaxContractSizeBytes passes initial validation, the body
// runs, and the deploy commits through the chunked-row path (its serialized
// row is exactly kMaxSerializedContractBytes). Pins the accept side of the
// boundary whose reject side is RejectsAttachedContractAboveSizeLimit; a
// > -> >= drift would reject transactions other validators accept.
TEST_P(SelectiveDeploymentTest, DeploysContractAtExactSizeLimit)
{
  Contract limit_contract(ContractType::kRv32);
  limit_contract.rv32_contract().data.resize_uninitialized(
      kMaxContractSizeBytes);
  for (size_t i = 0; i < limit_contract.rv32_contract().data.size(); ++i) {
    limit_contract.rv32_contract().data[i] =
        static_cast<uint8_t>((i * 37) + 11);
  }
  auto limit_hash = sha256(limit_contract);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 98;
    return nonce;
  }();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(0,
                [limit_hash](
                    NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto deploy_result =
                      ctx.handler().deploy_contract(kDeployNonce, limit_hash);
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
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto deployer_contract = make_native_contract(0);
  write_contract(deployer_contract);
  auto deployer_addr = make_contract_address(1);
  write_deployment(deployer_addr, deployer_contract);

  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(deployer_addr, kDeployNonce,
                                                   limit_hash);
  setup_grant_storage_token(deployer_addr, 1'000'000'000);
  setup_grant_storage_token(deployed_addr, 1'000'000'000);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = deployer_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
          .contracts_to_deploy = {limit_contract},
      }};
  auto handler = make_new_system_call_handler(handler_tx, context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();

  auto lookup = lookup_contract_from_db(limit_hash);
  ASSERT_TRUE(lookup.has_value());
  EXPECT_EQ(sha256(lookup.value()), limit_hash);
}

// An attached contract blob above kMaxContractSizeBytes is rejected
// deterministically during initial validation: the body never runs and
// nothing is deployed (#2569).
TEST_P(SelectiveDeploymentTest, RejectsAttachedContractAboveSizeLimit)
{
  Contract oversized(ContractType::kRv32);
  oversized.rv32_contract().data.resize_uninitialized(kMaxContractSizeBytes +
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
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

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

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, SelectiveDeploymentTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
