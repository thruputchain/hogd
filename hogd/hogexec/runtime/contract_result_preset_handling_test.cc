#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

constexpr ContractAddress kTestContractAddr =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kCallerContractAddr =
    NativeContractTestFramework::make_contract_address(1);

struct TestContract {
  constexpr static std::string_view kTraitName = "TestContract";
  constexpr static std::string_view kVersion = "0.0.0";

  constexpr static auto kExportNames =
      std::array<std::string_view, 2>{"return_ok_void", "return_ok_int"};

  using export_types = std::tuple<void (*)(), int32_t (*)()>;
};

using test_contract_client_t = ContractTraitClient<TestContract>;

// Second contract that calls the methods from TestContract
struct CallerContract {
  constexpr static std::string_view kTraitName = "CallerContract";
  constexpr static std::string_view kVersion = "0.0.0";

  constexpr static auto kExportNames =
      std::array<std::string_view, 2>{"call_return_ok_void",
                                      "call_return_ok_int"};

  using export_types = std::tuple<void (*)(), void (*)()>;
};

using caller_contract_client_t = ContractTraitClient<CallerContract>;

std::unique_ptr<NativeContract>
make_test_contract()
{
  NativeContractBuilder builder;
  builder.add<TestContract, "return_ok_void">(
      []() -> InvokeStatus<void> { return {}; });
  builder.add<TestContract, "return_ok_int">(
      []() -> InvokeStatus<int32_t> { return {123}; });
  return builder.build();
}

std::unique_ptr<NativeContract>
make_caller_contract()
{
  NativeContractBuilder builder;

  // Call return_ok_void method from TestContract
  builder.add<CallerContract, "call_return_ok_void">(
      [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        test_contract_client_t client(kTestContractAddr);
        auto [result, _gas] =
            client.template invoke_opt<"return_ok_void">(ctx.handler());
        if (!result.has_value() || !result.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  // Call return_ok_int method from TestContract
  builder.add<CallerContract, "call_return_ok_int">(
      [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        test_contract_client_t client(kTestContractAddr);
        auto [result, _gas] =
            client.template invoke_opt<"return_ok_int">(ctx.handler());
        if (!result.has_value() || !result.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (result.value().value() != 123) {
          ADD_FAILURE() << "Return value is not the expected value";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  return builder.build();
}

} // namespace

class ContractResultPresetHandlingTest : public ::testing::Test,
                                         public NativeContractTestFramework {
public:
  ContractResultPresetHandlingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kMockInMemory)
  {}

protected:
  void SetUp() override { initialize(); }

  static std::unique_ptr<RuntimeContext> make_context()
  {
    auto test_contract = make_test_contract();
    auto caller_contract = make_caller_contract();

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(test_contract));
    contract_map.emplace(1, std::move(caller_contract));

    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  void initialize()
  {
    // Set up contract addresses

    // Create and deploy the test contract (the one with the result methods)
    auto test_contract_xdr = make_native_contract(0);
    write_contract(test_contract_xdr);
    write_deployment(kTestContractAddr, test_contract_xdr);

    // Create and deploy the caller contract (calls methods from test contract)
    auto caller_contract_xdr = make_native_contract(1);
    write_contract(caller_contract_xdr);
    write_deployment(kCallerContractAddr, caller_contract_xdr);

    // Grant storage tokens to both contracts
    setup_grant_storage_token(kTestContractAddr, 1'000'000'000);
    setup_grant_storage_token(kCallerContractAddr, 1'000'000'000);
    context_ = make_context();
  }

  caller_contract_client_t caller_contract_client_ =
      caller_contract_client_t(kCallerContractAddr);

  std::unique_ptr<RuntimeContext> context_;
};

TEST_F(ContractResultPresetHandlingTest, TestGetOkVoid)
{
  initialize();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      caller_contract_client_
                          .template make_invocation<"call_return_ok_void">(0)}},
      *context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_F(ContractResultPresetHandlingTest, TestGetOkInt)
{
  initialize();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      caller_contract_client_
                          .template make_invocation<"call_return_ok_int">(0)}},
      *context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

} // namespace hogd
