#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "standard_asset_events.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class BalanceCostTrackingTest : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  BalanceCostTrackingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    // Set up the preset native token contract
    auto token_contract = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractNativeToken));
    write_contract(token_contract);
    write_deployment(kNativeTokenContractAddress, token_contract);
  }

  static std::tuple<int64_t, uint64_t> make_amount(int128_t amount)
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
  // kSenderAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost (the asserted receiver balance is
  // unaffected).
  setup_grant_storage_token(kSenderAddr,
                            kInitialBalance + replay_cache_cost_charged(1));

  // Create a contract that will transfer tokens
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto r_transfer = co_await native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr,
            make_amount(static_cast<int128_t>(kTransferAmount)));

        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  // Invoke the transfer
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler =
      make_new_system_call_handler(handler_tx, *test_runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

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
  // kSenderAddr is the root invoked address and commits 2 txs; top up its grant
  // by the replay-cache storage cost (the asserted receiver balance is
  // unaffected).
  setup_grant_storage_token(kSenderAddr,
                            kInitialBalance + replay_cache_cost_charged(2));

  // Create a contract that will transfer tokens
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto r_transfer = co_await native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr,
            make_amount(static_cast<int128_t>(kFirstTransferAmount)));

        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  fns.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto r_transfer = co_await native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr,
            make_amount(static_cast<int128_t>(kSecondTransferAmount)));

        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  // First transaction: transfer to receiver (new address)
  auto handler1_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler1 =
      make_new_system_call_handler(handler1_tx, *test_runtime_context);

  auto res1 = handler1.root_invoke()();
  ASSERT_TRUE(res1.is_root_success());

  auto commit_res1 = handler1.try_commit_transaction()();
  ASSERT_TRUE(commit_res1 == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler1.emit_events(ExecResult::ExecResultOK,
                           ProposedTransaction{.signed_tx = handler1_tx})());

  // Second transaction in the same block: transfer to same receiver
  auto handler2_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler2 =
      make_new_system_call_handler(handler2_tx, *test_runtime_context);

  auto res2 = handler2.root_invoke()();
  ASSERT_TRUE(res2.is_root_success());

  auto commit_res2 = handler2.try_commit_transaction()();
  ASSERT_TRUE(commit_res2 == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler2.emit_events(ExecResult::ExecResultOK,
                           ProposedTransaction{.signed_tx = handler2_tx})());

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

  // Grant initial tokens to sender (exactly balance_cost, plus the replay-cache
  // storage cost it owes as the root invoked address committing 1 tx). The
  // receiver still nets 0 because the full balance_cost is transferred.
  setup_grant_storage_token(kSenderAddr,
                            balance_cost + replay_cache_cost_charged(1));

  // Amounts: we'll transfer (balance_cost - 1) first, then 1
  int128_t kFirstTransferAmount = balance_cost - 1;
  ASSERT_GT(kFirstTransferAmount, 0);
  int128_t kSecondTransferAmount = 1;

  // Create a contract that orchestrates the transfers
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [kFirstTransferAmount, kSecondTransferAmount](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        // 1. Try to transfer (balance_cost - 1) in try_invoke frame -
        // should fail because insufficient to cover balance cost
        auto calldata1 = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr,
            make_amount(static_cast<int128_t>(kFirstTransferAmount)));
        CopyableInvokeOutputs r1_outputs{};
        auto r1 = co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr,
              std::as_bytes(std::span(calldata1.data(), calldata1.size())),
              std::span<const std::byte>{}, inputs, r1_outputs);
        }();

        if (!r1.has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Should fail in try_invoke (insufficient funds for balance cost): the
        // child frame runs (code None) but does not co_return normally — it
        // reverts or faults, so frame_status is not Return.
        if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r1_outputs.frame_status == InvocationFrameStatusCode::Return) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // 2. Call transfer with same arguments, NOT in try_invoke frame
        // - should succeed (creates the balance)
        auto r2 = co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          CopyableInvokeOutputs outputs{};
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr,
              std::as_bytes(std::span(calldata1.data(), calldata1.size())),
              std::span<const std::byte>{}, inputs, outputs);
        }();

        if (!r2.has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // 3. Call transfer again with remainder (1), in try_invoke
        // frame - should succeed
        auto calldata2 = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr,
            make_amount(static_cast<int128_t>(kSecondTransferAmount)));
        auto r3 = co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr,
              std::as_bytes(std::span(calldata2.data(), calldata2.size())),
              std::span<const std::byte>{}, inputs, CopyableInvokeOutputs{});
        }();

        if (!r3.has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  // Invoke the contract
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler =
      make_new_system_call_handler(handler_tx, *test_runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

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

// Regression test for #1503: a deferred negative delta on the sender's
// balance item (staged at the handler layer because the public transfer is
// now strict) must surface as a Deterministic error on a subsequent
// get_balance in the same transaction rather than aborting on an assertion
// (debug) or wrapping to a huge unsigned value (release).
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
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        // The public transfer path is strict now; stage the deferred negative
        // delta directly at the handler layer (the surface #1503 guards) to
        // keep the read-side fault and commit-time rejection covered.
        auto r_debit = co_await ctx.handler().modify_native_token_balance(
            ctx.handler().get_current_address(), -kTransferAmount);
        if (!r_debit.has_value() || !r_debit.value().has_value()) {
          ADD_FAILURE() << "staging the direct overspend debit failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto r_credit =
            co_await ctx.handler().modify_native_token_balance(kReceiverAddr,
                                                               kTransferAmount);
        if (!r_credit.has_value() || !r_credit.value().has_value()) {
          ADD_FAILURE() << "staging the direct overspend credit failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Reading sender's balance must now surface as a deterministic
        // fault: the child frame runs but traps, so the invoke syscall returns
        // kSyscallErrorCodeNone with frame_status == Fault.
        auto calldata_get = erc20_meta::make_calldata<u8"get_balance">();
        CopyableInvokeOutputs r_get_outputs{};
        auto r_get =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"get_balance">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_get, std::span<const std::byte>{},
              inputs, r_get_outputs);
        }();

        if (!r_get.has_value() ||
            r_get.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r_get_outputs.frame_status != InvocationFrameStatusCode::Fault) {
          ADD_FAILURE() << "get_balance after overspend did not co_return a "
                           "deterministic fault";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 11;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  // HogDb's commit-time balance-lock must still reject the transaction because
  // the sender's persisted balance would go negative; try_commit_transaction
  // internally aborts on failure.
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_NE(commit_res, ExecResult::ExecResultOK);
}

// Companion to NegativeBalanceReadbackReturnsDeterministic that exercises the
// get_balance_of read-site (which takes an explicit contract address). The
// overspend is staged at the handler layer because the public transfer path
// is now strict.
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
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        // The public transfer path is strict now; stage the deferred negative
        // delta directly at the handler layer (the surface #1503 guards) to
        // keep the read-side fault and commit-time rejection covered.
        auto r_debit = co_await ctx.handler().modify_native_token_balance(
            ctx.handler().get_current_address(), -kTransferAmount);
        if (!r_debit.has_value() || !r_debit.value().has_value()) {
          ADD_FAILURE() << "staging the direct overspend debit failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto r_credit =
            co_await ctx.handler().modify_native_token_balance(kReceiverAddr,
                                                               kTransferAmount);
        if (!r_credit.has_value() || !r_credit.value().has_value()) {
          ADD_FAILURE() << "staging the direct overspend credit failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto calldata_get =
            erc20_meta::make_calldata<u8"get_balance_of">(kSenderAddr);
        CopyableInvokeOutputs r_get_outputs{};
        auto r_get =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"get_balance_of">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_get, std::span<const std::byte>{},
              inputs, r_get_outputs);
        }();

        if (!r_get.has_value() ||
            r_get.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            r_get_outputs.frame_status != InvocationFrameStatusCode::Fault) {
          ADD_FAILURE() << "get_balance_of after overspend did not co_return a "
                           "deterministic fault";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 12;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_NE(commit_res, ExecResult::ExecResultOK);
}

// Flash-loan regression: with the strict invoke-time check, an overspending
// transfer ABORTS (instead of succeeding and leaving a deferred negative
// delta), a get_balance in the same transaction returns the untouched
// balance (frame Return, not Fault), and the transaction commits cleanly.
TEST_F(BalanceCostTrackingTest, OverspendTransferAbortsAndBalanceReadsClean)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(164);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(165);

  auto const *cost_model = get_default_cost_model();
  // kInitialBalance is the expected committed sender balance after the tx.
  // The actual grant includes replay_cache_cost so the tx commits cleanly.
  static int128_t kInitialBalance = cost_model->get_balance_cost() + 100;
  static int128_t kGrantAmount = kInitialBalance + replay_cache_cost_charged(1);
  // kTransferAmount exceeds the full grant so the strict in-tx check fires.
  static int128_t kTransferAmount = kGrantAmount + 1;
  setup_grant_storage_token(kSenderAddr, kGrantAmount);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata_transfer = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr, make_amount(kTransferAmount));
        CopyableInvokeOutputs transfer_outputs{};
        auto r_transfer =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_transfer,
              std::span<const std::byte>{}, inputs, transfer_outputs);
        }();
        if (!r_transfer.has_value() ||
            r_transfer.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            transfer_outputs.frame_status != InvocationFrameStatusCode::Abort) {
          ADD_FAILURE() << "overspending transfer did not abort at invoke";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // The aborted child staged nothing: reading the balance in the same
        // transaction succeeds (Return, not the #1503 Fault).
        auto calldata_get = erc20_meta::make_calldata<u8"get_balance">();
        CopyableInvokeOutputs get_outputs{};
        auto r_get =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"get_balance">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_get, std::span<const std::byte>{},
              inputs, get_outputs);
        }();
        if (!r_get.has_value() ||
            r_get.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            get_outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE()
              << "get_balance after aborted overspend did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 12;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  // Nothing bad was staged: the transaction commits.
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();

  // Sender's committed balance equals kInitialBalance: only replay-cache cost
  // was deducted; the aborted transfer staged nothing.
  auto sender_balance_opt = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kSenderAddr));
  ASSERT_TRUE(sender_balance_opt.has_value());
  EXPECT_EQ(from_xdr(sender_balance_opt.value().nnint()), kInitialBalance);
}

// Sequential debit regression: a successful transfer must reduce the balance
// seen by a later transfer in the same transaction. If the second transfer
// exceeds that remainder, only the second child frame aborts; the first
// transfer and its event remain committed.
TEST_F(BalanceCostTrackingTest,
       SecondInTxTransferOverRemainingBalanceAbortsAlone)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(220);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(221);

  auto const *cost_model = get_default_cost_model();
  static int128_t kBalanceCost = cost_model->get_balance_cost();
  static int128_t kFirstTransfer = kBalanceCost + 100;
  static int128_t kCommittedSenderRemainder = 50;
  static int128_t kReplayCost = replay_cache_cost_charged(1);
  static int128_t kGrantAmount =
      kFirstTransfer + kCommittedSenderRemainder + kReplayCost;
  static int128_t kInTxRemainder = kGrantAmount - kFirstTransfer;
  static int128_t kSecondTransfer = kInTxRemainder + 1;
  setup_grant_storage_token(kSenderAddr, kGrantAmount);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto first = co_await client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr, make_amount(kFirstTransfer));
        if (!first.has_value() || !first.value().has_value() ||
            !first.value().value().has_value() ||
            !first.value().value().value().is_ok()) {
          ADD_FAILURE() << "first in-tx transfer did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto second = co_await client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr, make_amount(kSecondTransfer));
        if (!second.has_value() || !second.value().has_value() ||
            !second.value().value().has_value() ||
            second.value().value().value().is_ok()) {
          ADD_FAILURE() << "second in-tx transfer did not abort on the reduced "
                           "balance";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto balance = co_await client.invoke_opt<u8"get_balance">(ctx);
        if (!balance.has_value() || !balance.value().has_value() ||
            !balance.value().value().has_value() ||
            !balance.value().value().value().is_ok()) {
          ADD_FAILURE() << "get_balance after contained second transfer failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto amount = balance.value().value().value().ok();
        uint128_t observed =
            (static_cast<uint128_t>(std::get<0>(amount)) << 64) |
            std::get<1>(amount);
        auto const expected = static_cast<uint128_t>(kInTxRemainder);
        if (observed != expected) {
          ADD_FAILURE() << "failed second transfer changed the in-tx balance";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 39;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  EphemeralTxIdentifier tx_id;
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();

  auto sender_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kSenderAddr));
  ASSERT_TRUE(sender_balance.has_value());
  EXPECT_EQ(from_xdr(sender_balance->nnint()), kCommittedSenderRemainder);

  auto receiver_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kReceiverAddr));
  ASSERT_TRUE(receiver_balance.has_value());
  EXPECT_EQ(from_xdr(receiver_balance->nnint()), 100);

  auto event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(event_log.has_value());
  EXPECT_EQ(event_log->events.size(), 1U)
      << "only the successful first transfer should emit an event";
}

// Pins the native-side sequential in-tx semantics: tokens received earlier in
// the same transaction are spendable by the receiver in that same transaction
// (available = base + net in-tx delta). The guest-side counterpart is the
// SDK's try_get_values_force behaviour.
TEST_F(BalanceCostTrackingTest, TransferFundedByInTxCreditSucceeds)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(166);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(167);

  auto const *cost_model = get_default_cost_model();
  static int128_t kBalanceCost = cost_model->get_balance_cost();
  // Transfer covers the receiver's one-time balance-creation cost plus 50
  // tokens, so the receiver can immediately spend the 50 back.
  static int128_t kTransferToReceiver = 50 + kBalanceCost;
  // Sender starts with enough for two balance items plus overhead and
  // replay-cache cost so the commit succeeds.
  static int128_t kInitialBalance = (2 * kBalanceCost) + 100;
  setup_grant_storage_token(kSenderAddr,
                            kInitialBalance + replay_cache_cost_charged(1));

  // Receiver contract: method 0 transfers 50 tokens back to kSenderAddr.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      receiver_fns;
  receiver_fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata_back =
            erc20_meta::make_calldata<u8"transfer">(kSenderAddr,
                                                    make_amount(int128_t{50}));
        CopyableInvokeOutputs back_outputs{};
        auto r_back =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_back, std::span<const std::byte>{},
              inputs, back_outputs);
        }();
        if (!r_back.has_value() ||
            r_back.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            back_outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "receiver back-transfer did not return";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto receiver_contract =
      std::make_unique<NativeContract>(std::move(receiver_fns));

  // Sender contract: (a) transfer kTransferToReceiver to receiver, then (b)
  // invoke receiver's method 0 so it spends the just-received tokens.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> sender_fns;
  sender_fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        // (a) Transfer to fresh receiver; the amount covers its balance-cost.
        auto calldata_fwd = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr, make_amount(kTransferToReceiver));
        CopyableInvokeOutputs fwd_outputs{};
        auto r_fwd =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_fwd, std::span<const std::byte>{},
              inputs, fwd_outputs);
        }();
        if (!r_fwd.has_value() ||
            r_fwd.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            fwd_outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "sender->receiver transfer did not return";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // (b) Invoke receiver's method 0; it spends tokens received above.
        CopyableInvokeOutputs invoke_outputs{};
        auto r_invoke =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              kReceiverAddr, std::span<const std::byte>{},
              std::span<const std::byte>{}, inputs, invoke_outputs);
        }();
        if (!r_invoke.has_value() ||
            r_invoke.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            invoke_outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "receiver method 0 did not return";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto sender_contract =
      std::make_unique<NativeContract>(std::move(sender_fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 12;
  constexpr int32_t kReceiverNativeId = 13;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(kReceiverNativeId, std::move(receiver_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto receiver_contract_xdr = make_native_contract(kReceiverNativeId);
  write_contract(receiver_contract_xdr);
  write_deployment(kReceiverAddr, receiver_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();
}

// Regression guard: non-overspending transfer followed by get_balance must
// still succeed end-to-end. Guards against overly aggressive tightening of the
// read-site check.
TEST_F(BalanceCostTrackingTest, BalanceReadbackAfterNonOverspendStillSucceeds)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(164);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(165);

  // Both sender and receiver end with a positive (tiny) residual balance so
  // HogDb's commit-time balance-lock accepts both.
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);
  static int128_t kTransferAmount = balance_cost + 1;
  static int128_t kInitialBalance = kTransferAmount + 1;
  // kSenderAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost so the (tiny) residual balance stays
  // positive and the commit still succeeds.
  setup_grant_storage_token(kSenderAddr,
                            kInitialBalance + replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_addr = kNativeTokenContractAddress;
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata_transfer = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr, make_amount(kTransferAmount));
        auto r_transfer =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_transfer,
              std::span<const std::byte>{}, inputs, CopyableInvokeOutputs{});
        }();

        if (!r_transfer.has_value() ||
            r_transfer.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "transfer did not co_return success";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto calldata_get = erc20_meta::make_calldata<u8"get_balance">();
        auto r_get =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"get_balance">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              native_token_addr, calldata_get, std::span<const std::byte>{},
              inputs, CopyableInvokeOutputs{});
        }();

        if (!r_get.has_value() ||
            r_get.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "get_balance after non-overspend did not "
                           "co_return success";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 13;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

// Regression test for #1807: in-transaction get_balance_of on a fresh receiver
// returns the gross transfer amount. The committed balance after block
// advancement is reduced by balance_cost. This is the intended optimistic
// execution semantic — runtime reads reflect pending deltas, and HogDb applies
// storage costs only at commit time.
TEST_F(BalanceCostTrackingTest, FreshReceiverInTxReadReturnsGrossAmount)
{
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  constexpr static ContractAddress kSenderAddr = make_contract_address(210);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(211);

  static int128_t kTransferAmount = balance_cost + 42;
  // kSenderAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost (the asserted receiver balances, in-tx and
  // committed, are unaffected).
  setup_grant_storage_token(kSenderAddr,
                            kTransferAmount + 1 + replay_cache_cost_charged(1));

  static uint128_t observed_receiver_balance = 0;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto r_transfer = co_await native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr, make_amount(kTransferAmount));

        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer to fresh receiver failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r_balance =
            co_await native_token_client.invoke_opt<u8"get_balance_of">(
                ctx, kReceiverAddr);

        if (!r_balance.has_value() || !r_balance.value().has_value() ||
            !r_balance.value().value().has_value() ||
            !r_balance.value().value().value().is_ok()) {
          ADD_FAILURE() << "get_balance_of fresh receiver failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto ok = r_balance.value().value().value().ok();
        observed_receiver_balance =
            (static_cast<uint128_t>(std::get<0>(ok)) << 64) |
            uint128_t{std::get<1>(ok)};
        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 20;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  EXPECT_EQ(observed_receiver_balance, static_cast<uint128_t>(kTransferAmount));

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto receiver_balance_addrkey =
      make_free_native_token_balance_addrkey(kReceiverAddr);
  auto receiver_balance_opt =
      lookup_storage_item_from_db(receiver_balance_addrkey);
  ASSERT_TRUE(receiver_balance_opt.has_value());
  int128_t committed_balance = from_xdr(receiver_balance_opt.value().nnint());
  EXPECT_EQ(committed_balance, kTransferAmount - balance_cost);
}

// Regression test for #1807: transferring exactly balance_cost to a fresh
// receiver. The in-tx readback shows balance_cost (gross), but the committed
// balance is 0 because the entire transfer covers the new row's storage cost.
TEST_F(BalanceCostTrackingTest,
       FreshReceiverExactCostTransferReadsGrossCommitsZero)
{
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  constexpr static ContractAddress kSenderAddr = make_contract_address(212);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(213);

  static int128_t kTransferAmount = 0;
  kTransferAmount = balance_cost;
  // kSenderAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost (the asserted receiver balances, in-tx and
  // committed, are unaffected).
  setup_grant_storage_token(kSenderAddr,
                            balance_cost + 1 + replay_cache_cost_charged(1));

  static uint128_t observed_receiver_balance = 0;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto r_transfer = co_await native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr, make_amount(kTransferAmount));

        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer to fresh receiver failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r_balance =
            co_await native_token_client.invoke_opt<u8"get_balance_of">(
                ctx, kReceiverAddr);

        if (!r_balance.has_value() || !r_balance.value().has_value() ||
            !r_balance.value().value().has_value() ||
            !r_balance.value().value().value().is_ok()) {
          ADD_FAILURE() << "get_balance_of fresh receiver failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto ok = r_balance.value().value().value().ok();
        observed_receiver_balance =
            (static_cast<uint128_t>(std::get<0>(ok)) << 64) |
            uint128_t{std::get<1>(ok)};
        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 21;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  EXPECT_EQ(observed_receiver_balance, static_cast<uint128_t>(balance_cost));

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto receiver_balance_addrkey =
      make_free_native_token_balance_addrkey(kReceiverAddr);
  auto receiver_balance_opt =
      lookup_storage_item_from_db(receiver_balance_addrkey);
  ASSERT_TRUE(receiver_balance_opt.has_value());
  int128_t committed_balance = from_xdr(receiver_balance_opt.value().nnint());
  EXPECT_EQ(committed_balance, 0);
}

// Regression test: the balance deltas forwarded to the scheduler on block
// commit must account for the storage cost charged when a balance is created
// for the first time. Previously the commit path (block_tx.cc) forwarded the
// raw delta via BalanceLock::get_delta(), which omitted the balance creation
// cost, so the scheduler's recorded delta for a fresh receiver was higher than
// the value actually committed to the DB by balance_cost.
TEST_F(BalanceCostTrackingTest, NewBalanceDeltaToSchedulerDeductsBalanceCost)
{
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  constexpr static ContractAddress kSenderAddr = make_contract_address(220);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(221);

  constexpr int128_t kInitialBalance = 1'000'000;
  constexpr int128_t kTransferAmount = 500'000;
  // kSenderAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost.
  setup_grant_storage_token(kSenderAddr,
                            kInitialBalance + replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto native_token_client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto r_transfer = co_await native_token_client.invoke_opt<u8"transfer">(
            ctx, kReceiverAddr,
            make_amount(static_cast<int128_t>(kTransferAmount)));

        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 1;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto test_runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler =
      make_new_system_call_handler(handler_tx, *test_runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_TRUE(commit_res == ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  // The block in which the transfer was committed is the current block; capture
  // it before advancing so we can query the deltas forwarded to the scheduler.
  auto committed_height = static_cast<BlockHeight>(get_block_number());

  advance_block();

  auto native_tokens_transferred =
      get_native_tokens_transferred(committed_height);

  // The receiver is a brand-new balance, so the delta forwarded to the
  // scheduler must be discounted by the balance creation cost to match the
  // value actually committed to the DB.
  ASSERT_TRUE(native_tokens_transferred.contains(kReceiverAddr));
  EXPECT_EQ(native_tokens_transferred.at(kReceiverAddr),
            kTransferAmount - balance_cost);

  // The sender is a pre-existing balance, so its delta is unaffected by the
  // balance creation cost -- but it is the invoked address, so it also pays the
  // replay-nonce cost, which is folded into its delta to the scheduler.
  ASSERT_TRUE(native_tokens_transferred.contains(kSenderAddr));
  EXPECT_EQ(native_tokens_transferred.at(kSenderAddr),
            -kTransferAmount - replay_cache_cost_charged(1));
}

// Characterization test (issue #2135): the native-token precompile treats the
// all-zero address as an ordinary balance key, so transfer(ContractAddress{},
// amount) credits the zero address rather than burning. The zero-recipient
// transfer leaves total_supply() unchanged and emits a standard
// Transfer(sender, zero, amount) event --- a real burn goes through burn() and
// emits its own Burn event. This is working
// as intended: it locks in that the native token does NOT special-case the
// zero address as a supply-reducing burn.
TEST_F(BalanceCostTrackingTest,
       NativeTokenZeroRecipientTransferCreditsZeroAddressWithoutReducingSupply)
{
  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);

  constexpr static ContractAddress kSenderAddr = make_contract_address(170);
  constexpr static ContractAddress kZeroAddr{};
  static int128_t kTransferAmount = 0;
  kTransferAmount = balance_cost + 10;
  setup_grant_storage_token(kSenderAddr, kTransferAmount + 1'000'000);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto const expected_total_supply = make_amount(int128_t{0});

        // No supply record exists in this direct-storage test setup.
        auto ts_before = co_await client.invoke_opt<u8"total_supply">(ctx);
        if (!ts_before.has_value() || !ts_before.value().has_value() ||
            !ts_before.value().value().has_value() ||
            !ts_before.value().value().value().is_ok() ||
            ts_before.value().value().value().ok() != expected_total_supply) {
          ADD_FAILURE() << "total_supply before transfer was not zero";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Transfer to the all-zero address. This is accepted, not rejected.
        auto r_transfer = co_await client.invoke_opt<u8"transfer">(
            ctx, kZeroAddr, make_amount(kTransferAmount));
        if (!r_transfer.has_value() || !r_transfer.value().has_value() ||
            !r_transfer.value().value().has_value() ||
            !r_transfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer(zero, amount) did not co_return Ok";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // total_supply() is unchanged: the native token does not burn.
        auto ts_after = co_await client.invoke_opt<u8"total_supply">(ctx);
        if (!ts_after.has_value() || !ts_after.value().has_value() ||
            !ts_after.value().value().has_value() ||
            !ts_after.value().value().value().is_ok() ||
            ts_after.value().value().value().ok() != expected_total_supply) {
          ADD_FAILURE() << "total_supply changed after zero-recipient transfer";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 14;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_contract_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_contract_xdr);
  write_deployment(kSenderAddr, sender_contract_xdr);

  EphemeralTxIdentifier tx_id;
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // The zero address is a fresh balance row, credited the transferred amount
  // minus the one-time balance-creation cost --- exactly like any other new
  // receiver.
  auto zero_balance_addrkey = make_free_native_token_balance_addrkey(kZeroAddr);
  auto zero_balance_opt = lookup_storage_item_from_db(zero_balance_addrkey);
  ASSERT_TRUE(zero_balance_opt.has_value());
  int128_t zero_balance = from_xdr(zero_balance_opt.value().nnint());
  EXPECT_EQ(zero_balance, kTransferAmount - balance_cost);

  // A standard Transfer(sender, zero, amount) event was emitted by the preset
  // native token contract --- the same shape as any transfer, with no
  // burn-specific handling.
  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().events.size(), 1);

  auto const &event = tx_event_log.value().events[0];
  EXPECT_EQ(event.contract_address, kNativeTokenContractAddress);
  EXPECT_EQ(event.num_topics, 3U);
  auto expected_event =
      make_event_from_cxs_struct(native_token_transfer_event_signature(),
                                 TransferEvent{
                                     .from_address = kSenderAddr,
                                     .to_address = kZeroAddr,
                                     .amount = to_xdr(kTransferAmount),
                                 });
  ASSERT_TRUE(expected_event);
  ASSERT_EQ(event.serialized_event.size(), expected_event->size());
  EXPECT_TRUE(std::equal(event.serialized_event.begin(),
                         event.serialized_event.end(),
                         expected_event->begin()));
}

// Regression for #3056: handler-side balance mutations bypassed the syscall
// gas channel. Measured on warm repeat transfers, since the cold-key
// kDBRPCCost would swamp the charge under test.
TEST_F(BalanceCostTrackingTest, RepeatTransfersChargeForBothBalanceMutations)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(181);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(182);
  static constexpr int128_t kTransferAmount = 10'000;
  static constexpr uint32_t kTransferCount = 3;
  // The debit and the credit, each priced as an NNInt add.
  static constexpr GasAmount kMutationFloor = 2 * kStorageAccessCost;

  setup_grant_storage_token(kSenderAddr, (kTransferAmount * kTransferCount) +
                                             replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr, make_amount(static_cast<int128_t>(kTransferAmount)));

        std::array<GasAmount, kTransferCount> gas_per_transfer{};
        for (uint32_t i = 0; i < kTransferCount; ++i) {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          CopyableInvokeOutputs outputs{};

          auto before = ctx.total_gas_consumed();
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kNativeTokenContractAddress,
              std::as_bytes(std::span(calldata.data(), calldata.size())),
              std::span<const std::byte>{}, inputs, outputs);
          gas_per_transfer[i] = ctx.total_gas_consumed() - before;

          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              outputs.frame_status != InvocationFrameStatusCode::Return) {
            ADD_FAILURE() << "transfer " << i << " did not return successfully";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        // Both keys are loaded from the first transfer onward, so the later
        // transfers do identical work and must cost the same.
        EXPECT_EQ(gas_per_transfer[1], gas_per_transfer[2])
            << "warm transfer cost is not stable; the floor below is not "
               "measuring what it claims to";
        // The charge under test: without it a warm transfer pays only its
        // entry debit and its event crossing.
        EXPECT_GE(gas_per_transfer[1], kMutationFloor)
            << "transfer did not charge for both balance mutations";
        co_return InvokeStatus<void>{};
      });
  auto sender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 17;
  contract_map.emplace(kSenderNativeId, std::move(sender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_xdr);
  write_deployment(kSenderAddr, sender_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  // The transfers emit events; the handler must not be destroyed holding them.
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

// Regression for #4210: an underfunded read must fault before loading; a later
// funded read of the same address still pays the cold price (it would run warm
// if the starved read had loaded it for free).
TEST_F(BalanceCostTrackingTest, UnderfundedBalanceReadFaultsWithoutLoading)
{
  constexpr static ContractAddress kCallerAddr = make_contract_address(214);
  constexpr static ContractAddress kFreshAddr = make_contract_address(215);
  constexpr static GasAmount kTwoGas = 2;
  constexpr static GasAmount kCappedCharge = kSyscallBaseCost + kTwoGas;
  // The two reads of kFreshAddr, in order: starved then funded.
  constexpr static std::array<GasAmount, 2> kGasLimits{kTwoGas, UINT64_MAX};

  std::array<GasAmount, kGasLimits.size()> charges{};
  std::array<InvocationFrameStatusCode, kGasLimits.size()> statuses{};

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata =
            erc20_meta::make_calldata<u8"get_balance_of">(kFreshAddr);
        for (size_t i = 0; i < kGasLimits.size(); ++i) {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"get_balance_of">()),
              .gas_limit = kGasLimits[i],
              .flags = std::to_underlying(InvokeFlag::None),
          };
          CopyableInvokeOutputs outputs{};

          auto const before = ctx.total_gas_consumed();
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kNativeTokenContractAddress,
              std::as_bytes(std::span(calldata.data(), calldata.size())),
              std::span<const std::byte>{}, inputs, outputs);
          charges[i] = ctx.total_gas_consumed() - before;

          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "get_balance_of invoke " << i
                          << " was rejected before its child frame ran";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          statuses[i] = outputs.frame_status;
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kCallerNativeId = 19;
  contract_map.emplace(kCallerNativeId,
                       std::make_unique<NativeContract>(std::move(fns)));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(kCallerNativeId);
  write_contract(caller_xdr);
  write_deployment(kCallerAddr, caller_xdr);

  auto tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kCallerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(tx, *runtime_context);
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  // The starved read faulted, paying only the capped child allowance.
  EXPECT_EQ(statuses[0], InvocationFrameStatusCode::Fault);
  EXPECT_EQ(charges[0], kCappedCharge);
  EXPECT_LT(charges[0], kDBRPCCost);

  // The funded read of the same address still pays the cold-load price, proving
  // the starved read did not load the key for free.
  EXPECT_EQ(statuses[1], InvocationFrameStatusCode::Return);
  EXPECT_GE(charges[1], kDBRPCCost);
}

// Regression for #4374: a transfer that exhausts gas at the first balance
// debit must refuse the write rather than stage deltas and fault later.
TEST_F(BalanceCostTrackingTest,
       NativeTokenTransferRefusesBalanceWriteAfterGasExhaustion)
{
  constexpr static ContractAddress kSenderAddr = make_contract_address(216);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(217);
  constexpr static int128_t kSpendableBalance = 1'000'000;
  constexpr static int128_t kTransferAmount = 1;
  // transfer_impl(1) + GetMsgSender(kSyscallBaseCost) + the warm balance read
  // (kStorageAccessCost, no cold load_gas) leave exactly zero gas for the first
  // balance mutation, so its flat charge cannot be afforded.
  constexpr static GasAmount kChildGas =
      1 + kSyscallBaseCost + kStorageAccessCost;

  setup_grant_storage_token(kSenderAddr,
                            kSpendableBalance + replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        // Warm the sender's balance view so the attack transfer's read owes
        // only kStorageAccessCost, landing the child budget right at the debit.
        auto warm_calldata =
            erc20_meta::make_calldata<u8"get_balance_of">(kSenderAddr);
        CopyableInvokeInputs warm_inputs{
            .entrypoint_id =
                sdk_method_id(erc20_meta::entrypoint<u8"get_balance_of">()),
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::None),
        };
        CopyableInvokeOutputs warm_outputs{};
        auto warm = co_await ctx.syscall<SyscallNo::kInvoke>(
            kNativeTokenContractAddress,
            std::as_bytes(
                std::span(warm_calldata.data(), warm_calldata.size())),
            std::span<const std::byte>{}, warm_inputs, warm_outputs);
        if (!warm.has_value() ||
            warm.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            warm_outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "warming balance read failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto calldata = erc20_meta::make_calldata<u8"transfer">(
            kReceiverAddr,
            make_amount(static_cast<uint128_t>(kTransferAmount)));
        CopyableInvokeInputs inputs{
            .entrypoint_id =
                sdk_method_id(erc20_meta::entrypoint<u8"transfer">()),
            .gas_limit = kChildGas,
            .flags = std::to_underlying(InvokeFlag::TryInvoke),
        };
        CopyableInvokeOutputs outputs{};
        auto result = co_await ctx.syscall<SyscallNo::kInvoke>(
            kNativeTokenContractAddress,
            std::as_bytes(std::span(calldata.data(), calldata.size())),
            std::span<const std::byte>{}, inputs, outputs);
        if (!result.has_value() ||
            result.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "attack transfer invoke was rejected";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSenderNativeId = 20;
  contract_map.emplace(kSenderNativeId,
                       std::make_unique<NativeContract>(std::move(fns)));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto sender_xdr = make_native_contract(kSenderNativeId);
  write_contract(sender_xdr);
  write_deployment(kSenderAddr, sender_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);
  handler.enable_tracing();

  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  auto const *root_trace = handler.get_trace();
  ASSERT_NE(root_trace, nullptr);
  // system_calls[0] warms the view; system_calls[1] is the attack transfer.
  ASSERT_EQ(root_trace->system_calls.size(), 2U);
  auto const &attack_invoke = root_trace->system_calls[1].body.invoke();
  ASSERT_NE(attack_invoke.outputs, nullptr);
  ASSERT_NE(attack_invoke.outputs->child_trace, nullptr);
  auto const &child_trace = *attack_invoke.outputs->child_trace;

  // Must fault at the unaffordable debit and stage no balance write.
  ASSERT_EQ(child_trace.status.status_code(), InvocationFrameStatusCode::Fault);
  EXPECT_EQ(child_trace.storage_writes.size(), 0U);
}

// Regression for #4270: warm native-token balance reads skipped the flat
// kStorageAccessCost once the view was loaded and load_gas dropped to zero.
TEST_F(BalanceCostTrackingTest, WarmBalanceReadsChargeStorageAccess)
{
  constexpr static ContractAddress kCallerAddr = make_contract_address(183);

  setup_grant_storage_token(kCallerAddr,
                            1'000'000 + replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata =
            erc20_meta::make_calldata<u8"get_balance_of">(kCallerAddr);

        // The first read loads the view; reads 2 and 3 are warm (no load_gas)
        // but still owe the flat access cost.
        std::array<GasAmount, 3> read_costs{};
        for (size_t i = 0; i < read_costs.size(); ++i) {
          CopyableInvokeInputs inputs{
              .entrypoint_id =
                  sdk_method_id(erc20_meta::entrypoint<u8"get_balance_of">()),
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          CopyableInvokeOutputs outputs{};

          auto before = ctx.total_gas_consumed();
          auto status = co_await ctx.syscall<SyscallNo::kInvoke>(
              kNativeTokenContractAddress,
              std::as_bytes(std::span(calldata.data(), calldata.size())),
              std::span<const std::byte>{}, inputs, outputs);
          read_costs[i] = ctx.total_gas_consumed() - before;

          if (!status.has_value() ||
              status.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              outputs.frame_status != InvocationFrameStatusCode::Return) {
            ADD_FAILURE() << "balance read " << i
                          << " did not return successfully";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        // A warm read pays exactly the Invoke crossing, the method's one gas
        // unit, and a single flat access charge (no load_gas once warm).
        static constexpr GasAmount kWarmReadCost =
            kSyscallBaseCost + 1 + kStorageAccessCost;
        EXPECT_EQ(read_costs[1], kWarmReadCost)
            << "warm balance read is not priced at crossing + method + access";
        EXPECT_EQ(read_costs[2], kWarmReadCost);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kCallerNativeId = 18;
  contract_map.emplace(kCallerNativeId, std::move(caller_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(kCallerNativeId);
  write_contract(caller_xdr);
  write_deployment(kCallerAddr, caller_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kCallerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = kDefaultMaxTxGas,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
