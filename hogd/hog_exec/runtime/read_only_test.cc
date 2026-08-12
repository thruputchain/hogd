#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/contract_loader.h"
#include "built_contracts/itc_erc20.h"
#include "built_contracts/read_only_test.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class ReadOnlyTest : public ::testing::TestWithParam<TestDB>,
                     public NativeContractTestFramework {
public:
  ReadOnlyTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    write_contract(contract_);
    write_deployment(contract_addr_, contract_);
    setup_grant_storage_token(contract_addr_, 1'000'000'000);
  }

  std::shared_ptr<Contract> contract_ =
      hogd::load_contract("read_only_test.wasm");
  ContractAddress const contract_addr_ = make_contract_address(0);
  using read_only_test_client_t =
      ContractTraitClient<read_only_test::ReadOnlyTest>;
  read_only_test_client_t client_ = read_only_test_client_t(contract_addr_);
};

namespace {

bool
check_result(HostFnStatus<SyscallStatus<std::optional<std::tuple<>>>> res)
{
  return res.has_value() && res.value().has_value() &&
         res.value().value().has_value();
}

} // namespace

TEST_P(ReadOnlyTest, ReadOnlyBytestringSet)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto res = client_.invoke_opt<u8"read_only_bytestring_set">(ctx, false);
    if (!check_result(res)) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  fns.emplace(1, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto res =
        client_.invoke_opt<u8"read_only_bytestring_set",
                           std::to_underlying(InvokeFlag::ReadOnly)>(ctx, true);
    if (!check_result(res)) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    return std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_context.release(), true}},
            {ContractType::kNative, {native_context.get(), false}}});
  };

  auto context = make_context();

  auto native_contract_addr = make_contract_address(1);
  auto native_contract_xdr = make_native_contract(0);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);
  setup_grant_storage_token(native_contract_addr, 1'000'000'000);

  {
    // allowed in non-read-only mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = native_contract_addr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler = make_new_system_call_handler(handler_tx, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  {
    // disallowed in non-read-only mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = native_contract_addr,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler = make_new_system_call_handler(handler_tx, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

TEST_P(ReadOnlyTest, ReadOnlyLogEvent)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto res = client_.invoke_opt<u8"read_only_log_event">(ctx, false);
    if (!check_result(res)) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  fns.emplace(1, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto res =
        client_.invoke_opt<u8"read_only_log_event",
                           std::to_underlying(InvokeFlag::ReadOnly)>(ctx, true);
    if (!check_result(res)) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    return std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_context.release(), true}},
            {ContractType::kNative, {native_context.get(), false}}});
  };

  auto context = make_context();

  auto native_contract_addr = make_contract_address(1);
  auto native_contract_xdr = make_native_contract(0);
  write_contract(native_contract_xdr);
  write_deployment(native_contract_addr, native_contract_xdr);
  setup_grant_storage_token(native_contract_addr, 1'000'000'000);

  {
    // LogEvent allowed in non-read-only mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = native_contract_addr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler = make_new_system_call_handler(handler_tx, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
  }

  {
    // LogEvent blocked in read-only mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = native_contract_addr,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler = make_new_system_call_handler(handler_tx, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

TEST_P(ReadOnlyTest, ReadOnlyPropagation)
{
  // Root invokes the middle contract with InvokeFlag::ReadOnly. The middle
  // contract invokes the leaf with InvokeFlag::None (no ReadOnly on that
  // syscall). The leaf still observes is_read_only() == true: read-only mode
  // propagates to nested frames without re-specifying the flag on each invoke.
  static constexpr ContractAddress kAddressLeaf = make_contract_address(1);
  static constexpr ContractAddress kAddressMiddle = make_contract_address(2);
  static constexpr ContractAddress kAddressRoot = make_contract_address(3);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_leaf;
  fns_leaf.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        if (!ctx.handler().is_read_only()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto contract_leaf = std::make_unique<NativeContract>(std::move(fns_leaf));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_middle;
  fns_middle.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        if (!ctx.handler().is_read_only()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto res = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressLeaf,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto contract_middle =
      std::make_unique<NativeContract>(std::move(fns_middle));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_root;
  fns_root.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto res = [&]() {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::ReadOnly),
          };
          return ctx.syscall<SyscallNo::kInvoke>(kAddressMiddle,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        }();
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto contract_root = std::make_unique<NativeContract>(std::move(fns_root));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_leaf));
  contract_map.emplace(1, std::move(contract_middle));
  contract_map.emplace(2, std::move(contract_root));
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);

  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kWasmi, {wasm_context.get(), false}},
          {ContractType::kNative, {native_context.get(), false}},
      });

  ContractAddress addrs[] = {kAddressLeaf, kAddressMiddle, kAddressRoot};
  for (int i = 0; i < 3; ++i) {
    auto xdr = make_native_contract(i);
    write_contract(xdr);
    write_deployment(addrs[i], xdr);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  {
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = kAddressRoot,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler =
        make_new_system_call_handler(handler_tx, *multiplexed_context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// Verifies that read-only enforcement works on the real native token
// precompile. get_balance_of is read-only safe and should succeed;
// transfer calls modify_native_token_balance and log_event which now
// return errors in read-only mode, causing the transfer to fail.
TEST_P(ReadOnlyTest, ReadOnlyNativeTokenPrecompile)
{
  using amount_t = std::tuple<uint64_t, uint64_t>;
  auto make_amount = [](uint128_t amount) -> amount_t {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  };

  constexpr ContractAddress kCallerAddr = make_contract_address(10);
  constexpr ContractAddress kReceiverAddr = make_contract_address(11);
  constexpr ContractAddress kObserverAddr = make_contract_address(12);
  constexpr int128_t kInitialBalance = 1'000'000;
  const int128_t transfer_amount =
      100 + get_default_cost_model()->get_balance_cost();

  setup_grant_storage_token(kCallerAddr, kInitialBalance);
  setup_grant_storage_token(kObserverAddr, 12345);

  auto native_token_client = ContractTraitClient<itc_erc20::ERC20>(
      GenesisConfigurationLoader::kPresetNativeTokenAddress);

  // Method 0: invoke get_balance_of in read-only mode (should succeed)
  // Method 1: invoke transfer in read-only mode (should fail)
  // Method 2: invoke transfer in normal mode (should succeed)
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto res = native_token_client.invoke_opt<
            u8"get_balance_of", std::to_underlying(InvokeFlag::ReadOnly)>(
            ctx, kObserverAddr);
        if (!res.has_value() || !res.value().has_value() ||
            !res.value().value().has_value() ||
            !res.value().value().value().is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto balance = res.value().value().value().ok();
        if (balance != make_amount(12345)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_caller.emplace(
      1, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto res = native_token_client.invoke_opt<
            u8"transfer", std::to_underlying(InvokeFlag::ReadOnly)>(
            ctx, kReceiverAddr, make_amount(transfer_amount));
        // The invoke should fail because modify_native_token_balance
        // returns InvalidSyscallInput in read-only mode, which causes
        // the transfer to return a deterministic error. The frame
        // should revert.
        if (res.has_value() && res.value().has_value() &&
            res.value().value().has_value() &&
            res.value().value().value().is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_caller.emplace(
      2, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto res = native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr, make_amount(transfer_amount));
        if (!res.has_value() || !res.value().has_value() ||
            !res.value().value().has_value() ||
            !res.value().value().value().is_ok()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  constexpr int32_t kCallerNativeId = 100;
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kCallerNativeId, std::move(caller_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);

  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kWasmi, {wasm_context.get(), false}},
          {ContractType::kNative, {native_context.get(), false}},
      });

  auto caller_contract_xdr = make_native_contract(kCallerNativeId);
  write_contract(caller_contract_xdr);
  write_deployment(kCallerAddr, caller_contract_xdr);
  setup_grant_storage_token(kCallerAddr, 1'000'000'000);

  {
    // get_balance_of succeeds in read-only mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = kCallerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler =
        make_new_system_call_handler(handler_tx, *multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  {
    // transfer fails in read-only mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = kCallerAddr,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler =
        make_new_system_call_handler(handler_tx, *multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  {
    // transfer succeeds in normal (non-read-only) mode
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{.invoked_address = kCallerAddr,
                                     .entrypoint = entrypoint_from_method_id(2),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler =
        make_new_system_call_handler(handler_tx, *multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
  }
}

// Verifies that read-only enforcement works when a WASM contract invokes the
// native token precompile with the read-only flag. get_balance_of should
// succeed; transfer should fail because modify_native_token_balance is blocked
// in read-only mode.
TEST_P(ReadOnlyTest, WasmReadOnlyNativeTokenPrecompile)
{
  using meta = ContractTraitMeta<read_only_test::ReadOnlyTest>;

  constexpr ContractAddress kObserverAddr = make_contract_address(12);
  constexpr ContractAddress kReceiverAddr = make_contract_address(13);

  // Deploy the native token precompile so the WASM contract can invoke it
  auto token_contract_xdr = make_native_contract(std::to_underlying(
      HogexecPrecompiledNativeContracts::kNativeContractHogToken));
  write_contract(token_contract_xdr);
  write_deployment(GenesisConfigurationLoader::kPresetNativeTokenAddress,
                   token_contract_xdr);

  setup_grant_storage_token(kObserverAddr, 12345);

  // Set up multiplexed context with WASM and native runtimes
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto native_context =
      std::make_unique<NativeContext>(std::move(contract_map));
  auto wasm_context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);

  auto multiplexed_context = std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kWasmi, {wasm_context.get(), false}},
          {ContractType::kNative, {native_context.get(), false}},
      });

  {
    // WASM contract read-only invokes get_balance_of — should succeed
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation =
                Invocation{
                    .invoked_address = contract_addr_,
                    .entrypoint =
                        meta::entrypoint<u8"read_only_native_get_balance">(),
                    .gas_limit = UINT64_MAX,
                    .calldata =
                        meta::make_calldata<u8"read_only_native_get_balance">(
                            kObserverAddr)},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler =
        make_new_system_call_handler(handler_tx, *multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }

  {
    // WASM contract read-only invokes transfer — should fail (frame reverts),
    // but the WASM method handles the error and returns normally
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation =
                Invocation{
                    .invoked_address = contract_addr_,
                    .entrypoint =
                        meta::entrypoint<u8"read_only_native_transfer">(),
                    .gas_limit = UINT64_MAX,
                    .calldata =
                        meta::make_calldata<u8"read_only_native_transfer">(
                            kReceiverAddr)},
            .uniquifier = make_new_uniquifier(),
        }};
    auto handler =
        make_new_system_call_handler(handler_tx, *multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  }
}

// Verifies that when a contract self-invokes in read-only mode and the
// inner frame tries to deploy a contract, the output address is not zeroed.
TEST_P(ReadOnlyTest, ReadOnlyDeployOutputPreserved)
{
  using meta = ContractTraitMeta<read_only_test::ReadOnlyTest>;

  auto context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation =
              Invocation{
                  .invoked_address = contract_addr_,
                  .entrypoint =
                      meta::entrypoint<u8"read_only_deploy_output_preserved">(),
                  .gas_limit = UINT64_MAX,
                  .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context);
  handler.hack_dont_use_test_bypass_allow_reentrance();

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

// Checks that aliasing guards (returning kFault) are performed before the read
// only guard
TEST_P(ReadOnlyTest, AliasingBeforeReadOnlyCheck)
{
  using meta = ContractTraitMeta<read_only_test::ReadOnlyTest>;

  auto context = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation =
              Invocation{.invoked_address = contract_addr_,
                         .entrypoint = meta::entrypoint<
                             u8"read_only_aliasing_before_read_only_check">(),
                         .gas_limit = UINT64_MAX,
                         .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context);
  handler.hack_dont_use_test_bypass_allow_reentrance();

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ReadOnlyTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
