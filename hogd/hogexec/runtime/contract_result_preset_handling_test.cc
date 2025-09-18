#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

struct TestContract {
  constexpr static std::string_view kTraitName = "TestContract";
  constexpr static std::string_view kVersion = "0.0.0";

  // These will be sorted before being passed as indices to method id
  // derivation.
  constexpr static auto kExportNames =
      std::array<std::string_view, 4>{"return_ok_void", "return_ok_int",
                                      "return_err_void", "return_err_int"};

  using export_types =
      std::tuple<Result<void, int32_t> (*)(), Result<int32_t, int32_t> (*)(),
                 Result<void, void> (*)(), Result<int32_t, int32_t> (*)()>;
};

using test_contract_client_t = ContractTraitClient<TestContract>;

// Second contract that calls the methods from TestContract
struct CallerContract {
  constexpr static std::string_view kTraitName = "CallerContract";
  constexpr static std::string_view kVersion = "0.0.0";

  constexpr static auto kExportNames =
      std::array<std::string_view, 4>{"call_return_ok_void",
                                      "call_return_ok_int",
                                      "call_return_err_void",
                                      "call_return_err_int"};

  using export_types =
      std::tuple<void (*)(), void (*)(), void (*)(), void (*)()>;
};

using caller_contract_client_t = ContractTraitClient<CallerContract>;

std::unique_ptr<NativeContract>
make_test_contract()
{
  NativeContractBuilder builder;
  builder.add<TestContract, "return_ok_void">(
      []() -> InvokeStatus<Result<void, int32_t>> {
        return {Result<void, int32_t>::make_ok()};
      });
  builder.add<TestContract, "return_ok_int">(
      []() -> InvokeStatus<Result<int32_t, int32_t>> {
        return {Result<int32_t, int32_t>::make_ok(123)};
      });
  builder.add<TestContract, "return_err_void">(
      []() -> InvokeStatus<Result<void, void>> {
        return {Result<void, void>::make_err()};
      });
  builder.add<TestContract, "return_err_int">(
      []() -> InvokeStatus<Result<int32_t, int32_t>> {
        return {Result<int32_t, int32_t>::make_err(456)};
      });
  return builder.build();
}

std::unique_ptr<NativeContract>
make_caller_contract(ContractAddress test_contract_addr)
{
  NativeContractBuilder builder;

  // Call return_ok_void method from TestContract
  builder.add<CallerContract, "call_return_ok_void">(
      [test_contract_addr](SystemCallHandler &handler) -> InvokeStatus<void> {
        test_contract_client_t client(test_contract_addr);
        auto [result, _gas] = client.template invoke<"return_ok_void">(handler);
        if (!result.has_value() || !result.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!result.value().value().is_ok()) {
          ADD_FAILURE() << "Return value is not ok";
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  // Call return_ok_int method from TestContract
  builder.add<CallerContract, "call_return_ok_int">(
      [test_contract_addr](SystemCallHandler &handler) -> InvokeStatus<void> {
        test_contract_client_t client(test_contract_addr);
        auto [result, _gas] = client.template invoke<"return_ok_int">(handler);
        if (!result.has_value() || !result.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!result.value().value().is_ok()) {
          ADD_FAILURE() << "Return value is not ok";
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (result.value().value().ok() != 123) {
          ADD_FAILURE() << "Return value is not the expected value";
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  // Call return_err_void method from TestContract
  builder.add<CallerContract, "call_return_err_void">(
      [test_contract_addr](SystemCallHandler &handler) -> InvokeStatus<void> {
        test_contract_client_t client(test_contract_addr);
        auto [result, _gas] =
            client.template invoke<"return_err_void">(handler);
        if (!result.has_value() || !result.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!result.value().value().is_err()) {
          ADD_FAILURE() << "Return value is not err";
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  // Call return_err_int method from TestContract
  builder.add<CallerContract, "call_return_err_int">(
      [test_contract_addr](SystemCallHandler &handler) -> InvokeStatus<void> {
        test_contract_client_t client(test_contract_addr);
        auto [result, _gas] = client.template invoke<"return_err_int">(handler);
        if (!result.has_value() || !result.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!result.value().value().is_err()) {
          ADD_FAILURE() << "Return value is not err";
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (result.value().value().err() != 456) {
          ADD_FAILURE() << "Return err value is not the expected value";
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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

  void initialize()
  {
    // Set up contract addresses

    // Create and deploy the test contract (the one with the result methods)
    auto test_contract = make_test_contract();
    auto test_contract_xdr = make_native_contract(0);
    write_contract(test_contract_xdr);
    write_deployment(test_contract_addr_, test_contract_xdr);

    // Create and deploy the caller contract (calls methods from test contract)
    auto caller_contract = make_caller_contract(test_contract_addr_);
    auto caller_contract_xdr = make_native_contract(1);
    write_contract(caller_contract_xdr);
    write_deployment(caller_contract_addr_, caller_contract_xdr);

    // Set up the native contract map and runtime context
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(test_contract));
    contract_map.emplace(1, std::move(caller_contract));
    runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));

    // Grant storage tokens to both contracts
    setup_grant_storage_token(test_contract_addr_, 1'000'000'000);
    setup_grant_storage_token(caller_contract_addr_, 1'000'000'000);
  }

  ContractAddress test_contract_addr_ = make_contract_address(0);
  ContractAddress caller_contract_addr_ = make_contract_address(1);
  std::shared_ptr<NativeContext> runtime_context_;
  caller_contract_client_t caller_contract_client_ =
      caller_contract_client_t(caller_contract_addr_);
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
      runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);
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
      runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);
}

TEST_F(ContractResultPresetHandlingTest, TestGetErrVoid)
{
  initialize();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{.invocation =
                              caller_contract_client_.template make_invocation<
                                  "call_return_err_void">(0)}},
      runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);
}

TEST_F(ContractResultPresetHandlingTest, TestGetErrInt)
{
  initialize();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      caller_contract_client_
                          .template make_invocation<"call_return_err_int">(0)}},
      runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res.has_value());
  ASSERT_TRUE(*commit_res);
}

} // namespace hogd
