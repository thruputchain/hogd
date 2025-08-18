#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "test_contracts/read_only_test.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ReadOnlyTestRunner : public ::testing::TestWithParam<TestDB>,
                           public NativeContractTestFramework {
public:
  ReadOnlyTestRunner()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    write_contract(contract_);
    write_deployment(contract_addr_, sha256(*contract_));
    setup_grant_storage_token(contract_addr_, 1'000'000'000);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_ =
      make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  std::shared_ptr<Contract> contract_ =
      test::load_test_contract("read_only_test.wasm");
  ContractAddress const contract_addr_ = make_contract_address(0);
  using read_only_test_client_t = ContractTraitClient<ReadOnlyTest>;
  read_only_test_client_t client_ = read_only_test_client_t(contract_addr_);
};

TEST_P(ReadOnlyTestRunner, ReadOnlyBytestringSet)
{

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto [res, _gas] =
                 client_.invoke<"read_only_bytestring_set">(handler, false);
             if (!res.has_value() || !res.value().has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {1,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             auto [res, _gas] =
                 client_.invoke<"read_only_bytestring_set",
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagReadOnly)>(handler,
                                                                      true);
             if (!res.has_value() || !res.value().has_value()) {

               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kWasmi, wasm_runtime_context_},
          {ContractType::kNative, native_context}});

  auto native_contract_addr = make_contract_address(1);
  auto native_contract_xdr = make_native_contract(0);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));
  setup_grant_storage_token(native_contract_addr, 1'000'000'000);

  {
    // allowed in non-read-only mode
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }

  {
    // disallowed in non-read-only mode
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

TEST_P(ReadOnlyTestRunner, ReadOnlyPropagation)
{
  auto native_contract_addr = make_contract_address(1);
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             if (!handler.is_read_only()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {1,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // call 0, with no read-only flag added.
             if (!handler.is_read_only()) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             auto [res, _gas] =
                 handler.invoke(native_contract_addr, 0, UINT64_MAX, {},
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagNone));
             if (!res.has_value() || res.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
          {2,
           [&](SystemCallHandler &handler,
               NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // call 1, with read-only flag added.
             auto [res, _gas] =
                 handler.invoke(native_contract_addr, 1, UINT64_MAX, {},
                                std::to_underlying(
                                    InvokeFlag::kInvokeFlagReadOnly));
             if (!res.has_value() || res.value() != SyscallErrorCode::kNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }
             return {};
           }},
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kWasmi, wasm_runtime_context_},
          {ContractType::kNative, native_context}});

  auto native_contract_xdr = make_native_contract(0);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, sha256(*native_contract_xdr));
  setup_grant_storage_token(native_contract_addr, 1'000'000'000);

  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(2),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ReadOnlyTestRunner,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
