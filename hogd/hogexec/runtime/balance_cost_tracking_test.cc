#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class BalanceCostTrackingTest : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  BalanceCostTrackingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  void SetUp() override
  {
    // Set up the preset native token contract
    auto token_contract = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(token_contract);
    write_deployment(GenesisConfigurationLoader::kPresetNativeTokenAddress,
                     token_contract);
  }

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  static std::tuple<int64_t, uint64_t> make_difference(int128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// Transfer a large number of units of native token to an address that doesn't
// exist. This should succeed, but then after the block closes the value should
// be discounted the balance cost.
TEST_F(BalanceCostTrackingTest, TransferToNewAddressDeductsBalanceCost)
{

  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  // Create sender and receiver addresses
  constexpr static ContractAddress kSenderAddr = make_contract_address(100);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(101);

  // Grant initial tokens to sender (large amount)
  constexpr int128_t kInitialBalance = 1'000'000;
  constexpr int128_t kTransferAmount = 500'000;
  setup_grant_storage_token(kSenderAddr, kInitialBalance);

  // Create a contract that will transfer tokens
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto native_token_client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);

    auto [r_transfer, g] = native_token_client.invoke_opt<"transfer">(
        ctx.handler(), kReceiverAddr,
        make_amount(static_cast<uint128_t>(kTransferAmount)));
    ctx.consume_gas(g);

    if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
        !r_transfer.value().value().has_value() ||
        !r_transfer.value().value().value().is_ok()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  // Invoke the transfer
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *test_runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  // Advance block to commit the transaction
  advance_block();

  // Check receiver's balance - should be transfer amount minus balance cost
  auto receiver_balance_addrkey =
      make_free_native_token_balance_addrkey(kReceiverAddr);
  auto receiver_balance_opt =
      lookup_storage_item_from_db(receiver_balance_addrkey);
  ASSERT_TRUE(receiver_balance_opt.has_value());
  int128_t receiver_balance = from_xdr(receiver_balance_opt.value().nnint());
  EXPECT_EQ(receiver_balance, kTransferAmount - balance_cost);
}

// Two transactions in one block that transfer to the same new address. The
// cost of the balance item should only be paid once.
TEST_F(BalanceCostTrackingTest, TwoTransfersToSameNewAddressCostOnce)
{

  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  // Create sender and receiver addresses
  constexpr static ContractAddress kSenderAddr = make_contract_address(200);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(201);

  // Grant initial tokens to sender
  constexpr int128_t kInitialBalance = 1'000'000;
  constexpr int128_t kFirstTransferAmount = 300'000;
  constexpr int128_t kSecondTransferAmount = 200'000;
  setup_grant_storage_token(kSenderAddr, kInitialBalance);

  // Create a contract that will transfer tokens
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto native_token_client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);

    auto [r_transfer, _g] = native_token_client.invoke_opt<"transfer">(
        ctx.handler(), kReceiverAddr,
        make_amount(static_cast<uint128_t>(kFirstTransferAmount)));

    if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
        !r_transfer.value().value().value().is_ok()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  fns.emplace(1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto native_token_client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);

    auto [r_transfer, _g] = native_token_client.invoke_opt<"transfer">(
        ctx.handler(), kReceiverAddr,
        make_amount(static_cast<uint128_t>(kSecondTransferAmount)));

    if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
        !r_transfer.value().value().value().is_ok()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  // First transaction: transfer to receiver (new address)
  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *test_runtime_context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());

  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_TRUE(commit_res1 == ExecResult::ExecResultOK);
  ASSERT_TRUE(handler1.emit_events(ExecResult::ExecResultOK)());

  // Second transaction in the same block: transfer to same receiver
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(1),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *test_runtime_context);

  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());

  auto commit_res2 = handler2.try_commit_transaction();
  ASSERT_TRUE(commit_res2 == ExecResult::ExecResultOK);
  ASSERT_TRUE(handler2.emit_events(ExecResult::ExecResultOK)());

  // Advance block to commit both transactions
  advance_block();

  // Check receiver's balance - should be sum of both transfers minus balance
  // cost only once
  auto receiver_balance_addrkey =
      make_free_native_token_balance_addrkey(kReceiverAddr);
  auto receiver_balance_opt =
      lookup_storage_item_from_db(receiver_balance_addrkey);
  ASSERT_TRUE(receiver_balance_opt.has_value());
  int128_t receiver_balance = from_xdr(receiver_balance_opt.value().nnint());
  int128_t expected_balance =
      kFirstTransferAmount + kSecondTransferAmount - balance_cost;
  EXPECT_EQ(receiver_balance, expected_balance);
}

// Test try_invoke with balance cost tracking. In one transaction:
// 1. Try to transfer with insufficient tokens to cover the cost (in try_invoke
//    frame) - should fail
// 2. Call transfer with the same arguments, not in try_invoke frame - should
//    succeed
// 3. Call transfer again with the remainder to make a new balance, in
// try_invoke
//    frame - should succeed
// In the end, we only transfer the cost for a balance (so the observed balance
// should be 0)
TEST_F(BalanceCostTrackingTest, TryInvokeBalanceCostTracking)
{
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  // Create sender and receiver addresses
  constexpr static ContractAddress kSenderAddr = make_contract_address(50);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(51);

  // Grant initial tokens to sender (exactly balance_cost)
  setup_grant_storage_token(kSenderAddr, balance_cost);

  // Amounts: we'll transfer (balance_cost - 1) first, then 1
  int128_t kFirstTransferAmount = balance_cost - 1;
  ASSERT_GT(kFirstTransferAmount, 0);
  int128_t kSecondTransferAmount = 1;

  // Create a contract that orchestrates the transfers
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [kFirstTransferAmount, kSecondTransferAmount](
                  NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                auto native_token_addr =
                    GenesisConfigurationLoader::kPresetNativeTokenAddress;
                using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

                // 1. Try to transfer (balance_cost - 1) in try_invoke frame -
                // should fail because insufficient to cover balance cost
                auto calldata1 = erc20_meta::make_calldata<"transfer">(
                    kReceiverAddr,
                    make_amount(static_cast<uint128_t>(kFirstTransferAmount)));
                auto r1 = [&]() {
                  RuntimeMemoryLength output_return_buffer_len = 0,
                                      output_returned_auth_token_len = 0;
                  return ctx.syscall<SyscallNo::kInvoke>(
                      native_token_addr,
                      sdk_method_id(erc20_meta::entrypoint<"transfer">()),
                      UINT64_MAX, calldata1,
                      std::to_underlying(InvokeFlag::TryInvoke),
                      std::span<uint32_t>{}, output_return_buffer_len,
                      output_returned_auth_token_len);
                }();

                if (!r1.has_value()) {
                  return InvokeError{InvokeErrorCode::Deterministic};
                }
                // Should fail in try_invoke (insufficient funds for balance
                // cost)
                if (r1.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
                  return InvokeError{InvokeErrorCode::Deterministic};
                }

                // 2. Call transfer with same arguments, NOT in try_invoke frame
                // - should succeed (creates the balance)
                auto r2 = [&]() {
                  RuntimeMemoryLength output_return_buffer_len = 0,
                                      output_returned_auth_token_len = 0;
                  return ctx.syscall<SyscallNo::kInvoke>(
                      native_token_addr,
                      sdk_method_id(erc20_meta::entrypoint<"transfer">()),
                      UINT64_MAX, calldata1,
                      std::to_underlying(InvokeFlag::None),
                      std::span<uint32_t>{}, output_return_buffer_len,
                      output_returned_auth_token_len);
                }();

                if (!r2.has_value()) {
                  return InvokeError{InvokeErrorCode::Deterministic};
                }
                if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                  return InvokeError{InvokeErrorCode::Deterministic};
                }

                // 3. Call transfer again with remainder (1), in try_invoke
                // frame - should succeed
                auto calldata2 = erc20_meta::make_calldata<"transfer">(
                    kReceiverAddr,
                    make_amount(static_cast<uint128_t>(kSecondTransferAmount)));
                auto r3 = [&]() {
                  RuntimeMemoryLength output_return_buffer_len = 0,
                                      output_returned_auth_token_len = 0;
                  return ctx.syscall<SyscallNo::kInvoke>(
                      native_token_addr,
                      sdk_method_id(erc20_meta::entrypoint<"transfer">()),
                      UINT64_MAX, calldata2,
                      std::to_underlying(InvokeFlag::TryInvoke),
                      std::span<uint32_t>{}, output_return_buffer_len,
                      output_returned_auth_token_len);
                }();

                if (!r3.has_value()) {
                  return InvokeError{InvokeErrorCode::Deterministic};
                }
                if (r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                  return InvokeError{InvokeErrorCode::Deterministic};
                }

                return {};
              });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  // Invoke the contract
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *test_runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  // Advance block to commit the transaction
  advance_block();

  // Check receiver's balance - should be 0 (we transferred exactly balance_cost
  // total, which gets deducted)
  auto receiver_balance_addrkey =
      make_free_native_token_balance_addrkey(kReceiverAddr);
  auto receiver_balance_opt =
      lookup_storage_item_from_db(receiver_balance_addrkey);
  ASSERT_TRUE(receiver_balance_opt.has_value());
  int128_t receiver_balance = from_xdr(receiver_balance_opt.value().nnint());
  EXPECT_EQ(receiver_balance, 0);
}

// Regression test for #1503: overspending via transfer leaves a deferred
// negative delta on the sender's balance item. A subsequent get_balance in the
// same transaction must surface as a Deterministic error rather than aborting
// on an assertion (debug) or wrapping to a huge unsigned value (release).
TEST_F(BalanceCostTrackingTest, NegativeBalanceReadbackReturnsDeterministic)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(160);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(161);

  // Size amounts so the only commit-time rejection reason is the sender's
  // balance going negative: receiver's new-entry balance_cost is covered by
  // the incoming delta, and sender's delta overshoots its initial balance.
  // The lambda can't capture, so use function-local statics.
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);
  static int128_t kInitialBalance = balance_cost + 1;
  static int128_t kTransferAmount = balance_cost + kInitialBalance + 1;
  setup_grant_storage_token(kSenderAddr, kInitialBalance);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto native_token_addr =
        GenesisConfigurationLoader::kPresetNativeTokenAddress;
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    // Overspend via transfer: records deferred-negative delta on
    // sender, returns local success.
    auto calldata_transfer = erc20_meta::make_calldata<"transfer">(
        kReceiverAddr, make_amount(static_cast<uint128_t>(kTransferAmount)));
    auto r_transfer = [&]() {
      RuntimeMemoryLength output_return_buffer_len = 0,
                          output_returned_auth_token_len = 0;
      return ctx.syscall<SyscallNo::kInvoke>(
          native_token_addr,
          sdk_method_id(erc20_meta::entrypoint<"transfer">()), UINT64_MAX,
          calldata_transfer, std::to_underlying(InvokeFlag::None),
          std::span<uint32_t>{}, output_return_buffer_len,
          output_returned_auth_token_len);
    }();

    if (!r_transfer.has_value() ||
        r_transfer.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "overspending transfer did not return success";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Reading sender's balance must now surface as a deterministic
    // error (inner Deterministic becomes DeterministicWasm to caller).
    auto calldata_get = erc20_meta::make_calldata<"get_balance">();
    auto r_get = [&]() {
      RuntimeMemoryLength output_return_buffer_len = 0,
                          output_returned_auth_token_len = 0;
      return ctx.syscall<SyscallNo::kInvoke>(
          native_token_addr,
          sdk_method_id(erc20_meta::entrypoint<"get_balance">()), UINT64_MAX,
          calldata_get, std::to_underlying(InvokeFlag::None),
          std::span<uint32_t>{}, output_return_buffer_len,
          output_returned_auth_token_len);
    }();

    if (!r_get.has_value() ||
        r_get.value() != SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
      ADD_FAILURE() << "get_balance after overspend did not return "
                       "kDeterministicWasm";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 11;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  // HogDB's commit-time balance-lock must still reject the transaction because
  // the sender's persisted balance would go negative; try_commit_transaction
  // internally aborts on failure.
  auto commit_res = handler.try_commit_transaction();
  ASSERT_NE(commit_res, ExecResult::ExecResultOK);
}

// Companion to NegativeBalanceReadbackReturnsDeterministic that exercises the
// get_balance_of read-site (which takes an explicit contract address).
TEST_F(BalanceCostTrackingTest,
       NegativeBalanceReadbackGetBalanceOfReturnsDeterministic)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(162);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(163);

  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);
  static int128_t kInitialBalance = balance_cost + 1;
  static int128_t kTransferAmount = balance_cost + kInitialBalance + 1;
  setup_grant_storage_token(kSenderAddr, kInitialBalance);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto native_token_addr =
        GenesisConfigurationLoader::kPresetNativeTokenAddress;
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    auto calldata_transfer = erc20_meta::make_calldata<"transfer">(
        kReceiverAddr, make_amount(static_cast<uint128_t>(kTransferAmount)));
    auto r_transfer = [&]() {
      RuntimeMemoryLength output_return_buffer_len = 0,
                          output_returned_auth_token_len = 0;
      return ctx.syscall<SyscallNo::kInvoke>(
          native_token_addr,
          sdk_method_id(erc20_meta::entrypoint<"transfer">()), UINT64_MAX,
          calldata_transfer, std::to_underlying(InvokeFlag::None),
          std::span<uint32_t>{}, output_return_buffer_len,
          output_returned_auth_token_len);
    }();

    if (!r_transfer.has_value() ||
        r_transfer.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "overspending transfer did not return success";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    auto calldata_get =
        erc20_meta::make_calldata<"get_balance_of">(kSenderAddr);
    auto r_get = [&]() {
      RuntimeMemoryLength output_return_buffer_len = 0,
                          output_returned_auth_token_len = 0;
      return ctx.syscall<SyscallNo::kInvoke>(
          native_token_addr,
          sdk_method_id(erc20_meta::entrypoint<"get_balance_of">()), UINT64_MAX,
          calldata_get, std::to_underlying(InvokeFlag::None),
          std::span<uint32_t>{}, output_return_buffer_len,
          output_returned_auth_token_len);
    }();

    if (!r_get.has_value() ||
        r_get.value() != SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
      ADD_FAILURE() << "get_balance_of after overspend did not return "
                       "kDeterministicWasm";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 12;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_NE(commit_res, ExecResult::ExecResultOK);
}

// Regression guard: non-overspending transfer followed by get_balance must
// still succeed end-to-end. Guards against overly aggressive tightening of the
// read-site check.
TEST_F(BalanceCostTrackingTest, BalanceReadbackAfterNonOverspendStillSucceeds)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(164);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(165);

  // Both sender and receiver end with a positive (tiny) residual balance so
  // HogDB's commit-time balance-lock accepts both.
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);
  static int128_t kTransferAmount = balance_cost + 1;
  static int128_t kInitialBalance = kTransferAmount + 1;
  setup_grant_storage_token(kSenderAddr, kInitialBalance);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto native_token_addr =
        GenesisConfigurationLoader::kPresetNativeTokenAddress;
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    auto calldata_transfer = erc20_meta::make_calldata<"transfer">(
        kReceiverAddr, make_amount(static_cast<uint128_t>(kTransferAmount)));
    auto r_transfer = [&]() {
      RuntimeMemoryLength output_return_buffer_len = 0,
                          output_returned_auth_token_len = 0;
      return ctx.syscall<SyscallNo::kInvoke>(
          native_token_addr,
          sdk_method_id(erc20_meta::entrypoint<"transfer">()), UINT64_MAX,
          calldata_transfer, std::to_underlying(InvokeFlag::None),
          std::span<uint32_t>{}, output_return_buffer_len,
          output_returned_auth_token_len);
    }();

    if (!r_transfer.has_value() ||
        r_transfer.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "transfer did not return success";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    auto calldata_get = erc20_meta::make_calldata<"get_balance">();
    auto r_get = [&]() {
      RuntimeMemoryLength output_return_buffer_len = 0,
                          output_returned_auth_token_len = 0;
      return ctx.syscall<SyscallNo::kInvoke>(
          native_token_addr,
          sdk_method_id(erc20_meta::entrypoint<"get_balance">()), UINT64_MAX,
          calldata_get, std::to_underlying(InvokeFlag::None),
          std::span<uint32_t>{}, output_return_buffer_len,
          output_returned_auth_token_len);
    }();

    if (!r_get.has_value() ||
        r_get.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "get_balance after non-overspend did not "
                       "return success";
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    return {};
  });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 13;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
