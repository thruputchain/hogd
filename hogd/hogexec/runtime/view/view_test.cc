#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "auth_claim.hh"
#include "built_contracts/fibonacci.h"
#include "built_contracts/read_only_test.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/view/view.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "x23/cleanup.h"

namespace hogd {

class ViewTest : public ::testing::Test {
public:
  using fibonacci_meta = ContractTraitMeta<fibonacci::Fibonacci>;
  using read_only_meta = ContractTraitMeta<read_only_test::ReadOnlyTest>;

  static constexpr ContractAddress kFibAddress =
      NativeContractTestFramework::make_contract_address(1);
  static constexpr ContractAddress kReadOnlyAddress =
      NativeContractTestFramework::make_contract_address(2);

  void SetUp() override
  {
    auto fib_contract = fibonacci_meta::load_wasm();
    framework_.write_contract(fib_contract);
    framework_.write_deployment(kFibAddress, fib_contract);
    framework_.setup_grant_storage_token(kFibAddress, 1'000'000'000);

    auto ro_contract = read_only_meta::load_wasm();
    framework_.write_contract(ro_contract);
    framework_.write_deployment(kReadOnlyAddress, ro_contract);
    framework_.setup_grant_storage_token(kReadOnlyAddress, 1'000'000'000);
  }

protected:
  ExpectedViewResult run_view(TransactionViewQuery const &query)
  {
    static auto context = make_hogexec_context();
    return run_view(query, *context);
  }

  ExpectedViewResult run_view(TransactionViewQuery const &query,
                              RuntimeContext &context)
  {
    return View::run_view_transaction(
        *framework_.get_storage_snapshot_cache(),
        framework_.make_new_per_tx_db_proxy(
            std::numeric_limits<TransactionID>::max()),
        framework_.make_new_per_tx_event_server_proxy(
            std::numeric_limits<TransactionID>::max()),
        query, context);
  }

  static TransactionViewQuery make_query(Invocation invocation,
                                         AuthClaim auth_claim = {})
  {
    return TransactionViewQuery{
        .invocation = std::move(invocation),
        .parent_address = {},
        .auth_claim = std::move(auth_claim),
        .forwarded_tokens = {},
        .invoke_flags = {},
    };
  }

  static TransactionViewQuery make_query_with_parent(
      Invocation invocation, ContractAddress parent_address,
      AuthClaim auth_claim = {})
  {
    return TransactionViewQuery{
        .invocation = std::move(invocation),
        .parent_address = std::move(parent_address),
        .auth_claim = std::move(auth_claim),
        .forwarded_tokens = {},
        .invoke_flags = {},
    };
  }

  NativeContractTestFramework framework_;

  x23::Defer<> force_syscallhandler_commit_ =
      SystemCallHandler::force_commit_or_abort();
};

TEST_F(ViewTest, BasicViewReturnsOk)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 10U);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewIsRoot)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 5U);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewWithMsgSender)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 5U);

  auto result = run_view(make_query_with_parent(invocation, kReadOnlyAddress));
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewNonexistentContract)
{
  ContractAddress bad_address =
      NativeContractTestFramework::make_contract_address(99);
  ContractTraitClient<fibonacci::Fibonacci> client(bad_address);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 10U);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::InvalidInput);
}

TEST_F(ViewTest, ViewReentrantParentMatchesInvoked)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 5U);

  auto result = run_view(make_query_with_parent(invocation, kFibAddress));
  EXPECT_EQ(result.status(), TransactionViewStatus::InvalidInput);
  EXPECT_EQ(result.error_code(),
            SyscallErrorCode::kSyscallErrorCodeReentrance);
}

TEST_F(ViewTest, ViewContractSelfInvokeFaults)
{
  ContractTraitClient<read_only_test::ReadOnlyTest> client(kReadOnlyAddress);
  auto invocation = client.make_invocation<
      "read_only_aliasing_before_read_only_check">(UINT64_MAX);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::InvalidInput);
}

TEST_F(ViewTest, ViewNativeContractGetMsgSenderIsRoot)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             ContractAddress sender{};
             auto r = ctx.syscall<SyscallNo::kGetMsgSender>(sender);
             if (!r.has_value()) {
               ADD_FAILURE() << "get_msg_sender syscall failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (r.value() !=
                 SyscallErrorCode::kSyscallErrorCodeIsRootTransaction) {
               ADD_FAILURE() << "expected IsRootTransaction";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto wasm_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  auto context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kWasmi, {wasm_context.release(), true}},
          {ContractType::kNative, {native_context.get(), false}}});

  auto native_xdr =
      NativeContractTestFramework::make_native_contract(0);
  framework_.write_contract(native_xdr);
  framework_.write_deployment(kNativeAddr, native_xdr);
  framework_.setup_grant_storage_token(kNativeAddr, 1'000'000'000);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *context);
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewNativeContractGetMsgSenderWithParent)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kParentAddr =
      NativeContractTestFramework::make_contract_address(20);

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             ContractAddress sender{};
             auto r = ctx.syscall<SyscallNo::kGetMsgSender>(sender);
             if (!r.has_value()) {
               ADD_FAILURE() << "get_msg_sender syscall failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "expected successful get_msg_sender";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             EXPECT_EQ(sender, kParentAddr);
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto wasm_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  auto context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kWasmi, {wasm_context.release(), true}},
          {ContractType::kNative, {native_context.get(), false}}});

  auto native_xdr =
      NativeContractTestFramework::make_native_contract(0);
  framework_.write_contract(native_xdr);
  framework_.write_deployment(kNativeAddr, native_xdr);
  framework_.setup_grant_storage_token(kNativeAddr, 1'000'000'000);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query_with_parent(invocation, kParentAddr), *context);
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

} // namespace hogd
