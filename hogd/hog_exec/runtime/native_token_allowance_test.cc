#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/genesis_root.h"
#include "built_contracts/genesis_root.hh"
#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "standard_asset_events.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class NativeTokenAllowanceTest : public ::testing::Test,
                                 public NativeContractTestFramework {
public:
  NativeTokenAllowanceTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    auto token_contract = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
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

  static uint128_t amount_to_u128(std::tuple<int64_t, uint64_t> const &a)
  {
    return (static_cast<uint128_t>(std::get<0>(a)) << 64) |
           uint128_t{std::get<1>(a)};
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// 1. Fresh (owner, spender) pair: allowance() returns 0, not a deterministic
// fault.
TEST_F(NativeTokenAllowanceTest, FreshPairAllowanceReturnsZero)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(170);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(171);

  // kOwnerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);
                auto r =
                    co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                              kSpenderAddr);
                if (!r.has_value() || !r.value().has_value() ||
                    !r.value().value().has_value() ||
                    !r.value().value().value().is_ok() ||
                    amount_to_u128(r.value().value().value().ok()) != 0) {
                  ADD_FAILURE() << "fresh allowance did not co_return Ok(0)";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 21;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

// 2. First positive adjust_allowance on a fresh pair succeeds, and the
// subsequent allowance() returns the written value.
TEST_F(NativeTokenAllowanceTest, FirstAdjustAllowanceFromZeroSucceeds)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(172);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(173);

  // kOwnerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r_adj = co_await client.invoke_opt<u8"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(100));
        if (!r_adj.has_value() || !r_adj.value().has_value() ||
            !r_adj.value().value().has_value() ||
            !r_adj.value().value().value().is_ok()) {
          ADD_FAILURE() << "first adjust_allowance(+100) did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto r_al = co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                              kSpenderAddr);
        if (!r_al.has_value() || !r_al.value().has_value() ||
            !r_al.value().value().has_value() ||
            !r_al.value().value().value().is_ok() ||
            amount_to_u128(r_al.value().value().value().ok()) != 100) {
          ADD_FAILURE() << "allowance() after +100 adjust was not 100";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 22;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

// 3. transfer_from on a fresh (owner, spender) pair with zero allowance must
// fail deterministically (commit-time underflow on the allowance NNInt).
TEST_F(NativeTokenAllowanceTest, TransferFromWithoutAllowanceFails)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(174);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(175);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(176);

  setup_grant_storage_token(kOwnerAddr, 1'000'000);
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);
                auto r = co_await client.invoke_opt<u8"transfer_from">(
                    ctx, kOwnerAddr, kReceiverAddr, make_amount(50));
                (void)r;
                co_return InvokeStatus<void>{};
              });
  auto spender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSpenderNativeId = 23;
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSpenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_NE(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// 4. Two adjust_allowance invocations in one transaction accumulate.
TEST_F(NativeTokenAllowanceTest, AllowanceAccumulatesAcrossAdjusts)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(177);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(178);

  // kOwnerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);
                for (int128_t d : {100, 50}) {
                  auto r = co_await client.invoke_opt<u8"adjust_allowance">(
                      ctx, kSpenderAddr, make_difference(d));
                  if (!r.has_value() || !r.value().has_value() ||
                      !r.value().value().has_value() ||
                      !r.value().value().value().is_ok()) {
                    ADD_FAILURE() << "adjust_allowance step failed";
                    co_return InvokeError{InvokeErrorCode::Deterministic};
                  }
                }
                auto r_al =
                    co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                              kSpenderAddr);
                if (!r_al.has_value() || !r_al.value().has_value() ||
                    !r_al.value().value().has_value() ||
                    !r_al.value().value().value().is_ok() ||
                    amount_to_u128(r_al.value().value().value().ok()) != 150) {
                  ADD_FAILURE() << "accumulated allowance was not 150";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 24;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

// 5. transfer_from consumes allowance: owner grants 100, spender transfers 50,
// allowance becomes 50; a second transfer of 60 must fail at commit.
TEST_F(NativeTokenAllowanceTest, TransferFromDeductsFromAllowance)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(179);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(180);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(181);

  // kOwnerAddr commits 2 txs (Tx1, Tx3) and kSpenderAddr commits 1 (Tx2);
  // grant each enough to pay the replay-cache storage cost. (Tx4 fails to
  // commit, so it consumes no replay-cache entry.)
  setup_grant_storage_token(kOwnerAddr,
                            1'000'000 + replay_cache_cost_charged(2));
  setup_grant_storage_token(kSpenderAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost() * 4);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  fns_owner.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r = co_await client.invoke_opt<u8"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(100'000));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "grant +100000 failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns_owner.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r = co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                           kSpenderAddr);
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok() ||
            amount_to_u128(r.value().value().value().ok()) != 50'000) {
          ADD_FAILURE() << "residual allowance was not 50000";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r =
            co_await client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr,
                                                          kReceiverAddr,
                                                          make_amount(50'000));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer_from(50000) did not co_return Ok";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns_spender.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r =
            co_await client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr,
                                                          kReceiverAddr,
                                                          make_amount(60'000));
        (void)r;
        co_return InvokeStatus<void>{};
      });
  auto spender_contract =
      std::make_unique<NativeContract>(std::move(fns_spender));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 25;
  constexpr int32_t kSpenderNativeId = 26;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);
  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  // Tx 1: owner grants +100000.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 2: spender transfers 50000.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kSpenderAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 3: owner asserts allowance == 50000.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(1),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 4: spender tries transfer_from(60000), must fail at commit.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kSpenderAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(1),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_NE(h.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
}

// 6. A decrement past the running allowance is rejected at invoke rather than
// deferred to commit: the strict path checks (committed base + net in-tx
// delta) at the point of the add. The earlier increment stands on its own, so
// the transaction commits with the allowance at the granted value -- there is
// no accepted-but-invalid pending delta for a later sibling call to net away
// (hogd#1918). The netting sequence itself is pinned, against this precompile
// and the RV32 itc_erc20 reference side by side, by
// Erc20ParityTest.NegativeAllowanceNettingParity.
TEST_F(NativeTokenAllowanceTest, NegativeAdjustCannotDriveAllowanceBelowZero)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(182);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(183);
  constexpr static int128_t kGrant = 100;
  constexpr static int128_t kOverDecrement = -150;

  // kOwnerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost, so the commit assertion below turns
  // on the allowance semantics rather than on the invoked address's funding.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);

                auto grant = co_await client.invoke_opt<u8"adjust_allowance">(
                    ctx, kSpenderAddr, make_difference(kGrant));
                if (!grant.has_value() || !grant.value().has_value() ||
                    !grant.value().value().has_value() ||
                    !grant.value().value().value().is_ok()) {
                  ADD_FAILURE()
                      << "initial allowance grant did not co_return Ok";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }

                auto over = co_await client.invoke_opt<u8"adjust_allowance">(
                    ctx, kSpenderAddr, make_difference(kOverDecrement));
                if (!over.has_value() || !over.value().has_value() ||
                    !over.value().value().has_value()) {
                  ADD_FAILURE()
                      << "over-decrement faulted instead of returning an error";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                if (over.value().value().value().is_ok()) {
                  ADD_FAILURE()
                      << "a decrement past the running allowance must be "
                         "rejected at invoke, not deferred to commit";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }

                // The rejected decrement staged nothing: the grant is still
                // visible.
                auto allowance =
                    co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                              kSpenderAddr);
                if (!allowance.has_value() || !allowance.value().has_value() ||
                    !allowance.value().value().has_value() ||
                    !allowance.value().value().value().is_ok() ||
                    amount_to_u128(allowance.value().value().value().ok()) !=
                        static_cast<uint128_t>(kGrant)) {
                  ADD_FAILURE()
                      << "allowance after the rejected decrement is not the "
                         "granted value";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 27;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // DB readback: only the grant was committed.
  auto allowance_key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr,
                                                       kSpenderAddr);
  ASSERT_TRUE(allowance_key_data.has_value());
  StorageKey allowance_storage_key(StorageKeyType::ContractData);
  allowance_storage_key.contract_data_key() = sha256(*allowance_key_data);
  auto allowance_item = lookup_storage_item_from_db(
      AddressAndKey{.address = kNativeTokenContractAddress,
                    .key = std::move(allowance_storage_key)});
  ASSERT_TRUE(allowance_item.has_value());
  EXPECT_EQ(from_xdr(allowance_item->nnint()), kGrant);
}

// Regression test for #1628: self-transfer must not publish a zero delta
// to the scheduler (which would trip CHECK_NE(num_received, 0)).
TEST_F(NativeTokenAllowanceTest, SelfTransferDoesNotPublishZeroDelta)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(184);
  constexpr static int128_t kInitialBalance = 1'000'000;

  // kOwnerAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost so the post-commit balance assertion
  // (== kInitialBalance) still holds after the self-transfer nets to zero.
  setup_grant_storage_token(kOwnerAddr,
                            kInitialBalance + replay_cache_cost_charged(1));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);
                auto r =
                    co_await client.invoke_opt<u8"transfer">(ctx, kOwnerAddr,
                                                             make_amount(37));
                if (!r.has_value() || !r.value().has_value() ||
                    !r.value().value().has_value() ||
                    !r.value().value().value().is_ok()) {
                  ADD_FAILURE() << "self-transfer did not co_return Ok";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 28;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  const BlockHeight committed_block = get_block_number();
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto hogs_transferred = get_hogs_transferred(committed_block);
  // The zero-value self-transfer itself publishes no delta. The replay nonce
  // consumed by this transaction, however, charges its storage cost to the
  // invoked address, which is published to the scheduler -- so the only delta
  // is exactly that replay cost.
  EXPECT_EQ(hogs_transferred.size(), 1U)
      << "only the replay-nonce cost should be published for a zero-delta "
         "self-transfer";
  EXPECT_EQ(hogs_transferred.at(kOwnerAddr), -replay_cache_cost_charged(1));

  auto balance_item = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kOwnerAddr));
  ASSERT_TRUE(balance_item.has_value());
  EXPECT_EQ(from_xdr(balance_item->nnint()), kInitialBalance);
}

// 7. Strict allowance rejects an overflowing adjust_allowance at invoke time,
// mirroring itc_erc20's try_strict_inc. The allowance is checked against its
// committed snapshot value at the point of the add (not deferred to commit),
// so an increment that would push the committed allowance above INT128_MAX is
// aborted immediately and leaves the prior allowance intact. (Balance checks
// are also strict; see erc20_parity_test.cc for parity coverage.)
TEST_F(NativeTokenAllowanceTest, OverflowingAdjustAllowanceRejectsAtInvoke)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(190);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(191);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(192);
  constexpr static int128_t kSeedAllowance =
      std::numeric_limits<int128_t>::max() - 5;
  constexpr static int128_t kOverflowIncrement = 10;
  constexpr static uint128_t kTransferAmount = 10'000;

  // kOwnerAddr commits 2 txs (Tx1 seed, Tx2 adjust) and kSpenderAddr commits 1
  // tx (Tx3 transfer_from); grant each enough to pay the replay-cache cost.
  setup_grant_storage_token(kOwnerAddr,
                            1'000'000 + replay_cache_cost_charged(2));
  setup_grant_storage_token(kSpenderAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kReceiverAddr, 0);
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost() * 4);

  // Owner method 0: seed allowance for spender.
  // Owner method 1: adjust allowance for spender by +10 (must abort: overflow).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  fns_owner.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r = co_await client.invoke_opt<u8"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(kSeedAllowance));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "seed adjust_allowance failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns_owner.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r_adj = co_await client.invoke_opt<u8"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(kOverflowIncrement));
        // Strict allowance must reject the overflowing increment at invoke.
        if (r_adj.has_value() && r_adj.value().has_value() &&
            r_adj.value().value().has_value() &&
            r_adj.value().value().value().is_ok()) {
          ADD_FAILURE()
              << "overflowing adjust_allowance(+10) should be rejected";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  // Spender method 0: transfer_from kTransferAmount. The allowance is still the
  // seeded INT128_MAX - 5 (the +10 adjust aborted), so this succeeds.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r_xfer = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr, make_amount(kTransferAmount));
        if (!r_xfer.has_value() || !r_xfer.value().has_value() ||
            !r_xfer.value().value().has_value() ||
            !r_xfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer_from against seeded allowance should "
                           "succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto spender_contract =
      std::make_unique<NativeContract>(std::move(fns_spender));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 30;
  constexpr int32_t kSpenderNativeId = 33;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);
  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  // Tx 1: owner grants allowance to spender: INT128_MAX - 5.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 2: owner adjusts allowance by +10. Strict allowance aborts the add
  // (would overflow the committed INT128_MAX - 5); the owner method swallows
  // the abort and commits with the allowance unchanged.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(1),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 3: spender transfer_from(10000) against the intact INT128_MAX - 5
  // allowance succeeds.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kSpenderAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }
}

// Self-allowances are ordinary entries, matching the reference interface.
TEST_F(NativeTokenAllowanceTest, AdjustAllowanceForSelfSucceeds)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(196);

  // kOwnerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);
                auto r = co_await client.invoke_opt<u8"adjust_allowance">(
                    ctx, kOwnerAddr, make_difference(100));
                if (!r.has_value() || !r.value().has_value() ||
                    !r.value().value().has_value() ||
                    !r.value().value().value().is_ok()) {
                  ADD_FAILURE() << "adjust_allowance for self should succeed";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                auto allowance =
                    co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                              kOwnerAddr);
                if (!allowance || !*allowance || !**allowance ||
                    !(***allowance).is_ok() ||
                    amount_to_u128((***allowance).ok()) != 100) {
                  ADD_FAILURE() << "self allowance did not retain the grant";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 35;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
}

// 9. transfer_from where owner == caller skips allowance: no allowance
// row is created and no storage cost is consumed by the native token contract.
TEST_F(NativeTokenAllowanceTest, SelfTransferFromSkipsAllowanceAndNoStorageCost)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(197);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(198);
  // kOwnerAddr is the root invoked address and commits 1 tx; top up its grant
  // by the replay-cache storage cost. (The asserted native-token-contract
  // storage balance below is unaffected by the replay cost.)
  setup_grant_storage_token(kOwnerAddr,
                            1'000'000 + replay_cache_cost_charged(1));
  setup_grant_storage_token(kReceiverAddr, 0);
  setup_grant_storage_token(kNativeTokenContractAddress, 0);

  auto allowance_key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr, kOwnerAddr);
  ASSERT_TRUE(allowance_key_data.has_value());
  StorageKey allowance_storage_key(StorageKeyType::ContractData);
  allowance_storage_key.contract_data_key() = sha256(*allowance_key_data);
  AddressAndKey allowance_addrkey{.address = kNativeTokenContractAddress,
                                  .key = std::move(allowance_storage_key)};
  ASSERT_FALSE(lookup_storage_item_from_db(allowance_addrkey).has_value());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    kNativeTokenContractAddress);
                auto r = co_await client.invoke_opt<u8"transfer_from">(
                    ctx, kOwnerAddr, kReceiverAddr, make_amount(100));
                if (!r.has_value() || !r.value().has_value() ||
                    !r.value().value().has_value() ||
                    !r.value().value().value().is_ok()) {
                  ADD_FAILURE() << "self transfer_from should succeed";
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 36;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  EXPECT_FALSE(lookup_storage_item_from_db(allowance_addrkey).has_value())
      << "self transfer_from must not create an allowance row";

  auto native_token_balance_item = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kNativeTokenContractAddress));
  ASSERT_TRUE(native_token_balance_item.has_value());
  EXPECT_EQ(from_xdr(native_token_balance_item->nnint()), 0)
      << "native token contract storage balance must be unchanged";
}

// Characterization test (issue #2135, amendment #005): transfer_from(owner,
// ContractAddress{}, amount) by an allowance-authorized spender is accepted
// just like any other delegated transfer. The spender's allowance is consumed,
// the owner is debited, and the all-zero address is credited as an ordinary
// native-token balance row. Because the native token has no transfer-to-zero
// burn semantics, the zero recipient is NOT special-cased as a
// supply-reducing burn: total_supply() stays unchanged and a standard
// Transfer(owner, zero, amount) event is emitted; the native token has no
// burn entry point.
// This locks in the behavior as working-as-intended.
TEST_F(NativeTokenAllowanceTest,
       TransferFromToZeroConsumesAllowanceAndKeepsSupply)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(194);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(195);
  constexpr static ContractAddress kZeroAddr{};
  constexpr static int128_t kInitialOwnerBalance = 1'000'000;
  constexpr static int128_t kTransferAmount = 125'000;

  auto const *cost_model = get_default_cost_model();
  int128_t balance_cost = cost_model->get_balance_cost();
  ASSERT_GT(balance_cost, 0);
  // The fixed transfer amount must comfortably exceed the balance-creation
  // cost so the zero address ends with a positive committed balance; guard the
  // assumption so a future cost-model change fails here rather than confusingly
  // downstream.
  ASSERT_GT(kTransferAmount, balance_cost);
  const int128_t expected_zero_balance = kTransferAmount - balance_cost;

  // The owner is the root invoked address of the allowance-grant transaction,
  // which consumes one replay-cache entry. The owner-balance assertion below is
  // checked after that transaction (and the spender's transfer_from) but before
  // the owner's later view-check transaction, so cover exactly one entry here
  // so the assertion (which expects only the transfer to be debited) still
  // holds; the owner's remaining balance covers the second entry's reservation.
  // The spender is the root invoked address of transfer_from and pays for its
  // one replay-cache entry.
  setup_grant_storage_token(kOwnerAddr, kInitialOwnerBalance +
                                            replay_cache_cost_charged(1));
  setup_grant_storage_token(kSpenderAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            cost_model->get_nnint_cost() * 4);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  // Method 0: owner grants the spender an allowance for the transfer.
  fns_owner.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto delta = make_difference(kTransferAmount);
        auto r =
            co_await client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                             delta);
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "owner allowance grant failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  // Method 1: after the zero-recipient transfer_from, the allowance is fully
  // consumed and total_supply() is still zero because this direct-storage test
  // setup has no supply record.
  fns_owner.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto allowance =
            co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                      kSpenderAddr);
        if (!allowance.has_value() || !allowance.value().has_value() ||
            !allowance.value().value().has_value() ||
            !allowance.value().value().value().is_ok() ||
            amount_to_u128(allowance.value().value().value().ok()) != 0) {
          ADD_FAILURE() << "residual allowance was not consumed to zero";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto const expected_total_supply = make_amount(int128_t{0});
        auto total_supply = co_await client.invoke_opt<u8"total_supply">(ctx);
        if (!total_supply.has_value() || !total_supply.value().has_value() ||
            !total_supply.value().value().has_value() ||
            !total_supply.value().value().value().is_ok() ||
            total_supply.value().value().value().ok() !=
                expected_total_supply) {
          ADD_FAILURE() << "total_supply changed after zero-recipient transfer";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  // Method 0: spender transfers the owner's funds to the all-zero address.
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kZeroAddr,
            make_amount(static_cast<int128_t>(kTransferAmount)));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE()
              << "transfer_from(owner, zero, amount) did not co_return Ok";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto spender_contract =
      std::make_unique<NativeContract>(std::move(fns_spender));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 32;
  constexpr int32_t kSpenderNativeId = 33;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);
  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  // Tx 1: owner grants the spender an allowance.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 2: spender executes transfer_from(owner, zero, amount).
  EphemeralTxIdentifier spend_tx_id;
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kSpenderAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context, &spend_tx_id);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // The owner is debited the full transfer amount.
  auto owner_balance_item = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kOwnerAddr));
  ASSERT_TRUE(owner_balance_item.has_value());
  EXPECT_EQ(from_xdr(owner_balance_item->nnint()),
            kInitialOwnerBalance - kTransferAmount);

  // The all-zero address holds an ordinary fresh balance row, credited the
  // transferred amount minus the one-time balance-creation cost.
  auto zero_balance_item = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kZeroAddr));
  ASSERT_TRUE(zero_balance_item.has_value());
  EXPECT_EQ(from_xdr(zero_balance_item->nnint()), expected_zero_balance);

  // A standard Transfer(owner, zero, amount) event was emitted.
  auto tx_event_log = get_tx_event_log(spend_tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().exec_result, ExecResult::ExecResultOK);
  ASSERT_EQ(tx_event_log.value().events.size(), 1);

  auto const &event = tx_event_log.value().events[0];
  EXPECT_EQ(event.contract_address, kNativeTokenContractAddress);
  EXPECT_EQ(event.num_topics, 3U);
  auto expected_event =
      make_event_from_cxs_struct(native_token_transfer_event_signature(),
                                 TransferEvent{
                                     .from_address = kOwnerAddr,
                                     .to_address = kZeroAddr,
                                     .amount = to_xdr(kTransferAmount),
                                 });
  ASSERT_TRUE(expected_event);
  ASSERT_EQ(event.serialized_event.size(), expected_event->size());
  EXPECT_TRUE(std::equal(event.serialized_event.begin(),
                         event.serialized_event.end(),
                         expected_event->begin()));

  // Tx 3: confirm via the precompile views that the allowance is consumed and
  // native total_supply() is unchanged.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(1),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }
}

// 10. Over-balance transfer_from with ample allowance: the child frame Aborts
// (TooLarge on the owner balance check), the tx commits, and the committed
// allowance is unchanged -- the staged allowance debit reverted with the
// aborted frame.
TEST_F(NativeTokenAllowanceTest, OverBalanceTransferFromRevertsAllowanceDebit)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(200);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(201);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(202);
  // Allowance is ample (500), but the owner's residual balance after Tx1 is
  // only 50, so the balance check in transfer_from fires before the tokens
  // move.
  constexpr static int128_t kGrantAllowance = 500;
  constexpr static int128_t kTransferAmount = 500;
  constexpr static int128_t kOwnerResidualBalance = 50; // < kTransferAmount

  // Owner commits Tx1 (allowance grant) and needs replay-cache token.  After
  // Tx1 the owner's residual balance is kOwnerResidualBalance, which is
  // strictly less than kTransferAmount.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1) +
                                            kOwnerResidualBalance);
  // Spender commits Tx2 (transfer_from attempt) and needs replay-cache token.
  setup_grant_storage_token(kSpenderAddr, replay_cache_cost_charged(1));
  // Native token contract pays for the new allowance NNInt entry created in
  // Tx1; Tx2's staged debit reverts with the aborted frame so no extra cost.
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  // Owner method 0: grant the spender an allowance of kGrantAllowance.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  fns_owner.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r = co_await client.invoke_opt<u8"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(kGrantAllowance));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "adjust_allowance(+kGrantAllowance) did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  // Spender method 0: attempt transfer_from(owner, receiver, kTransferAmount).
  // The allowance check passes (kGrantAllowance >= kTransferAmount) but the
  // owner balance check fails (kOwnerResidualBalance < kTransferAmount), so
  // the child frame Aborts and the staged allowance debit reverts.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto r = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr,
            make_amount(static_cast<int128_t>(kTransferAmount)));
        // All three optional layers must be present: the syscall ran, the
        // frame ran and returned data, and the data deserialized. Any missing
        // layer indicates an unexpected low-level error.
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value()) {
          ADD_FAILURE()
              << "invoke_opt for transfer_from failed at syscall/frame level";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The child frame Aborts on over-balance, encoding as Result::Err.
        // Confirm the rejection; an unexpected Ok here means the transfer
        // succeeded despite insufficient balance.
        if (r.value().value().value().is_ok()) {
          ADD_FAILURE() << "expected transfer_from to reject over-balance "
                           "child frame but it returned Ok";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Result::Err confirmed -- child frame Aborted with
        // ERC20Error::TooLarge. Return {} so the spender's root frame completes
        // normally.
        co_return InvokeStatus<void>{};
      });
  auto spender_contract =
      std::make_unique<NativeContract>(std::move(fns_spender));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 37;
  constexpr int32_t kSpenderNativeId = 38;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);
  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  // Tx 1: owner grants allowance of kGrantAllowance to spender.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kOwnerAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // Tx 2: spender calls transfer_from for kTransferAmount. The child frame
  // Aborts (owner balance kOwnerResidualBalance < kTransferAmount); the
  // spender's outer frame returns normally and the tx commits.
  {
    auto h_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kSpenderAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx, *runtime_context);
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }

  advance_block();

  // DB readback: the committed allowance must still equal the original grant.
  // The staged allowance debit from the aborted transfer_from child frame
  // reverted with that frame and must not appear in the committed state.
  auto allowance_key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr,
                                                       kSpenderAddr);
  ASSERT_TRUE(allowance_key_data.has_value());
  StorageKey allowance_storage_key(StorageKeyType::ContractData);
  allowance_storage_key.contract_data_key() = sha256(*allowance_key_data);
  AddressAndKey allowance_addrkey{.address = kNativeTokenContractAddress,
                                  .key = std::move(allowance_storage_key)};
  auto allowance_item = lookup_storage_item_from_db(allowance_addrkey);
  ASSERT_TRUE(allowance_item.has_value())
      << "allowance NNInt entry must exist after Tx1";
  EXPECT_EQ(from_xdr(allowance_item->nnint()), kGrantAllowance)
      << "committed allowance must be unchanged after aborted transfer_from";
}

// An allowance granted by the owner is immediately visible to a nested
// spender in the same transaction. A successful delegated spend consumes part
// of it; a second spend one unit above the remainder aborts without disturbing
// the first spend or its allowance debit.
TEST_F(NativeTokenAllowanceTest,
       SameTransactionAllowanceGrantSpendAndContainedFailure)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(230);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(231);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(232);

  auto const *cost_model = get_default_cost_model();
  static int128_t kBalanceCost = cost_model->get_balance_cost();
  static int128_t kTransferAmount = kBalanceCost + 100;
  constexpr static int128_t kResidualAllowance = 25;
  static int128_t kGrantedAllowance = kTransferAmount + kResidualAllowance;
  static int128_t kSecondTransferAmount = kResidualAllowance + 1;
  constexpr static int128_t kCommittedOwnerRemainder = 50;
  static int128_t kOwnerGrant =
      kTransferAmount + kCommittedOwnerRemainder + replay_cache_cost_charged(1);

  setup_grant_storage_token(kOwnerAddr, kOwnerGrant);
  setup_grant_storage_token(kNativeTokenContractAddress,
                            cost_model->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> spender_fns;
  spender_fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);

        auto first = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr,
            make_amount(static_cast<int128_t>(kTransferAmount)));
        if (!first.has_value() || !first.value().has_value() ||
            !first.value().value().has_value() ||
            !first.value().value().value().is_ok()) {
          ADD_FAILURE() << "same-tx transfer_from did not consume the grant";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto second = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr,
            make_amount(static_cast<int128_t>(kSecondTransferAmount)));
        if (!second.has_value() || !second.value().has_value() ||
            !second.value().value().has_value() ||
            second.value().value().value().is_ok()) {
          ADD_FAILURE() << "second transfer_from did not abort on the "
                           "remaining allowance";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto allowance =
            co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                      kSpenderAddr);
        if (!allowance.has_value() || !allowance.value().has_value() ||
            !allowance.value().value().has_value() ||
            !allowance.value().value().value().is_ok() ||
            amount_to_u128(allowance.value().value().value().ok()) !=
                static_cast<uint128_t>(kResidualAllowance)) {
          ADD_FAILURE() << "failed second spend changed the residual allowance";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto spender_contract =
      std::make_unique<NativeContract>(std::move(spender_fns));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> owner_fns;
  owner_fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto grant = co_await client.invoke_opt<u8"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(kGrantedAllowance));
        if (!grant.has_value() || !grant.value().has_value() ||
            !grant.value().value().has_value() ||
            !grant.value().value().value().is_ok()) {
          ADD_FAILURE() << "same-tx allowance grant failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::None),
        };
        CopyableInvokeOutputs outputs{};
        auto invoke = co_await ctx.syscall<SyscallNo::kInvoke>(
            kSpenderAddr, std::span<const std::byte>{},
            std::span<const std::byte>{}, inputs, outputs);
        if (!invoke.has_value() ||
            invoke.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "nested spender did not co_return normally";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(owner_fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kOwnerNativeId = 40;
  constexpr int32_t kSpenderNativeId = 41;
  contract_map.emplace(kOwnerNativeId, std::move(owner_contract));
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto owner_xdr = make_native_contract(kOwnerNativeId);
  write_contract(owner_xdr);
  write_deployment(kOwnerAddr, owner_xdr);
  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  EphemeralTxIdentifier tx_id;
  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kOwnerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
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

  auto allowance_key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr,
                                                       kSpenderAddr);
  ASSERT_TRUE(allowance_key_data.has_value());
  StorageKey allowance_storage_key(StorageKeyType::ContractData);
  allowance_storage_key.contract_data_key() = sha256(*allowance_key_data);
  AddressAndKey allowance_addrkey{.address = kNativeTokenContractAddress,
                                  .key = std::move(allowance_storage_key)};
  auto allowance_item = lookup_storage_item_from_db(allowance_addrkey);
  ASSERT_TRUE(allowance_item.has_value());
  EXPECT_EQ(from_xdr(allowance_item->nnint()), kResidualAllowance);

  auto owner_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kOwnerAddr));
  ASSERT_TRUE(owner_balance.has_value());
  EXPECT_EQ(from_xdr(owner_balance->nnint()), kCommittedOwnerRemainder);

  auto receiver_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kReceiverAddr));
  ASSERT_TRUE(receiver_balance.has_value());
  EXPECT_EQ(from_xdr(receiver_balance->nnint()), 100);

  auto event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(event_log.has_value());
  EXPECT_EQ(event_log->events.size(), 2U)
      << "only the grant and successful delegated transfer should emit";
}

// A delegated zero transfer over fresh keys succeeds at invoke, including the
// zero allowance debit, but cannot commit: the two fresh native-balance rows
// each have to fund their own creation cost from a zero credit. Storage-cost
// parity with the contract-data-backed RV32 token is deliberately outside the
// ERC20 parity harness (which runs with zero costs), so pin this native
// accounting edge separately here.
TEST_F(NativeTokenAllowanceTest, ZeroTransferFromFreshKeysFailsAtCommit)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(233);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(234);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(235);

  setup_grant_storage_token(kSpenderAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto transfer =
            co_await client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr,
                                                          kReceiverAddr,
                                                          make_amount(0));
        if (!transfer.has_value() || !transfer.value().has_value() ||
            !transfer.value().value().has_value() ||
            !transfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "zero transfer_from over fresh keys failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto allowance =
            co_await client.invoke_opt<u8"allowance">(ctx, kOwnerAddr,
                                                      kSpenderAddr);
        if (!allowance.has_value() || !allowance.value().has_value() ||
            !allowance.value().value().has_value() ||
            !allowance.value().value().value().is_ok() ||
            amount_to_u128(allowance.value().value().value().ok()) != 0) {
          ADD_FAILURE() << "zero transfer_from changed the allowance value";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto spender_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kSpenderNativeId = 42;
  contract_map.emplace(kSpenderNativeId, std::move(spender_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto spender_xdr = make_native_contract(kSpenderNativeId);
  write_contract(spender_xdr);
  write_deployment(kSpenderAddr, spender_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address = kSpenderAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError);

  EXPECT_FALSE(lookup_storage_item_from_db(
                   make_free_native_token_balance_addrkey(kOwnerAddr))
                   .has_value());
  EXPECT_FALSE(lookup_storage_item_from_db(
                   make_free_native_token_balance_addrkey(kReceiverAddr))
                   .has_value());

  auto allowance_key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr,
                                                       kSpenderAddr);
  ASSERT_TRUE(allowance_key_data.has_value());
  StorageKey allowance_storage_key(StorageKeyType::ContractData);
  allowance_storage_key.contract_data_key() = sha256(*allowance_key_data);
  EXPECT_FALSE(lookup_storage_item_from_db(
                   AddressAndKey{.address = kNativeTokenContractAddress,
                                 .key = std::move(allowance_storage_key)})
                   .has_value());
}

TEST_F(NativeTokenAllowanceTest, GenesisRootRejectsDistributionAfterHeightOne)
{
  auto root_contract = load_stock_genesis_root_contract();
  write_contract(root_contract);
  write_deployment(kGenesisRootContractAddress, root_contract);

  advance_block();
  ASSERT_GT(get_block_number(), 1UL);

  ContractTraitClient<genesis_root::GenesisRoot> root_client(
      kGenesisRootContractAddress);
  auto tx = Transaction{
      .invocation = root_client.make_invocation<u8"distribute">(
          UINT64_MAX, x23::Vec<genesis_root::Distribution, UINT32_MAX>{},
          make_amount(0), make_amount(0)),
      .uniquifier = make_new_uniquifier(),
  };
  auto runtime_context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(tx, *runtime_context);
  EXPECT_FALSE(handler.root_invoke()().is_root_success());
}

// Pins HOG conservation (balance + locked storage <= supply <= INT128_MAX) so a
// storage refund against a near-max balance never trips the CHECK in
// release_balance(). Needed because the mint precompile stopped bounding supply
// below INT128_MAX at invoke (that moved to commit for rv32 parity). Mint max,
// write a bytestring, then delete it; a regression aborts on the delete.
TEST_F(NativeTokenAllowanceTest, StorageRefundCreditsAgainstNearMaxBalance)
{
  constexpr static ContractAddress kWhaleAddr = make_contract_address(180);
  constexpr static int128_t kMaxSupply = std::numeric_limits<int128_t>::max();

  auto const *cost_model = get_default_cost_model();
  const int128_t balance_cost = cost_model->get_balance_cost();

  // The genesis root roots the mint tx; the native token address owns the
  // freshly-created total-supply row.
  setup_grant_storage_token(kGenesisRootContractAddress,
                            replay_cache_cost_charged(1));
  setup_grant_storage_token(kNativeTokenContractAddress,
                            cost_model->get_nnint_cost());

  // Genesis-root driver: method 0 mints INT128_MAX to the whale.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> root_fns;
  root_fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto mint = co_await client.invoke_opt<u8"mint">(
            ctx, kWhaleAddr, make_amount(static_cast<int128_t>(kMaxSupply)));
        if (!mint.has_value() || !mint.value().has_value() ||
            !mint.value().value().has_value() ||
            !mint.value().value().value().is_ok()) {
          ADD_FAILURE() << "mint of INT128_MAX to the whale did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto root_contract = std::make_unique<NativeContract>(std::move(root_fns));

  // Whale contract: method 0 writes a bytestring; method 1 deletes it.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> whale_fns;
  whale_fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("whale_storage");
        Bytestring payload = make_bytestring("release me back to a near-max "
                                             "balance");
        auto r = co_await ctx.syscall<SyscallNo::kBytestringSet>(
            key, std::as_bytes(std::span(payload.data(), payload.size())));
        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "whale failed to write storage";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  whale_fns.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto key = make_storage_key("whale_storage");
        auto r = co_await ctx.syscall<SyscallNo::kDeleteObject>(key);
        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "whale failed to delete storage";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto whale_contract = std::make_unique<NativeContract>(std::move(whale_fns));

  constexpr int32_t kRootNativeId = 44;
  constexpr int32_t kWhaleNativeId = 45;
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kRootNativeId, std::move(root_contract));
  contract_map.emplace(kWhaleNativeId, std::move(whale_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto root_xdr = make_native_contract(kRootNativeId);
  write_contract(root_xdr);
  write_deployment(kGenesisRootContractAddress, root_xdr);
  auto whale_xdr = make_native_contract(kWhaleNativeId);
  write_contract(whale_xdr);
  write_deployment(kWhaleAddr, whale_xdr);

  auto run_block_tx = [&](ContractAddress invoked, Runtime::MethodID method) {
    auto tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = invoked,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(tx, *runtime_context);
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    ExecResult commit = handler.try_commit_transaction()();
    EXPECT_EQ(commit, ExecResult::ExecResultOK);
    EXPECT_TRUE(
        handler.emit_events(commit, ProposedTransaction{.signed_tx = tx})());
    advance_block();
    return commit;
  };

  // Block 1: mint INT128_MAX to the whale. Its stored balance is the mint
  // amount minus the one-time balance-row cost (self-funded from the mint).
  ASSERT_EQ(run_block_tx(kGenesisRootContractAddress, 0),
            ExecResult::ExecResultOK);
  auto whale_after_mint = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kWhaleAddr));
  ASSERT_TRUE(whale_after_mint.has_value());
  EXPECT_EQ(from_xdr(whale_after_mint->nnint()), kMaxSupply - balance_cost)
      << "the whale must hold essentially the entire INT128_MAX supply";

  // Block 2: the whale writes storage, locking cost out of its near-max
  // balance.
  ASSERT_EQ(run_block_tx(kWhaleAddr, 0), ExecResult::ExecResultOK);

  // Block 3: the whale deletes the storage. The released cost is credited back
  // via release_balance() against a base just below INT128_MAX. Reaching a
  // clean commit here (rather than a CHECK-abort) is the property under test.
  ASSERT_EQ(run_block_tx(kWhaleAddr, 1), ExecResult::ExecResultOK);

  // The storage row is gone, and the refund landed: the balance is back within
  // a hair of INT128_MAX (it dips only by the whale's own replay-cache costs
  // for the two txs it rooted -- far below this generous slack).
  auto whale_storage = lookup_storage_item_from_db(
      AddressAndKey{.address = kWhaleAddr, .key = [] {
                      StorageKey k(StorageKeyType::ContractData);
                      k.contract_data_key() = make_storage_key("whale_storage");
                      return k;
                    }()});
  EXPECT_FALSE(whale_storage.has_value())
      << "the whale's storage row must be deleted";

  auto whale_final = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kWhaleAddr));
  ASSERT_TRUE(whale_final.has_value());
  int128_t final_balance = from_xdr(whale_final->nnint());
  EXPECT_LE(final_balance, kMaxSupply - balance_cost);
  EXPECT_GT(final_balance, kMaxSupply - balance_cost - (int128_t{1} << 40))
      << "the storage refund must credit back against a near-max base";
}

// Regression for #2963: allowance key derivation must pay for its SHA-256.
TEST_F(NativeTokenAllowanceTest, AllowanceChargesForKeyDerivationSha256)
{
  constexpr static ContractAddress kCallerAddr = make_contract_address(219);
  constexpr static ContractAddress kOwnerAddr = make_contract_address(220);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(221);

  // kCallerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost.
  setup_grant_storage_token(kCallerAddr, replay_cache_cost_charged(1));

  auto key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr,
                                                       kSpenderAddr);
  ASSERT_TRUE(key_data.has_value());
  const GasAmount derivation_cost =
      kSyscallBaseCost + GasCost::sha256_hash(key_data->size());
  // Otherwise the assertion below would also pass on the unfixed code.
  ASSERT_GT(derivation_cost, 1U);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [derivation_cost](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

        auto calldata =
            erc20_meta::make_calldata<u8"allowance">(kOwnerAddr, kSpenderAddr);
        CopyableInvokeInputs inputs{
            .entrypoint_id =
                sdk_method_id(erc20_meta::entrypoint<u8"allowance">()),
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::None),
        };
        CopyableInvokeOutputs outputs{};

        auto before = ctx.total_gas_consumed();
        auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
            kNativeTokenContractAddress,
            std::as_bytes(std::span(calldata.data(), calldata.size())),
            std::span<const std::byte>{}, inputs, outputs);
        auto gas_used = ctx.total_gas_consumed() - before;

        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "allowance child invoke did not return successfully";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        // A floor: gas_used also covers the crossing and the child frame.
        EXPECT_GE(gas_used, derivation_cost)
            << "allowance did not charge for its SHA-256 key derivation";
        co_return InvokeStatus<void>{};
      });
  auto caller_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr int32_t kCallerNativeId = 41;
  contract_map.emplace(kCallerNativeId, std::move(caller_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
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
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto handler = make_new_system_call_handler(handler_tx, *runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

class NativeTokenCleanupTest : public NativeTokenAllowanceTest {
protected:
  static constexpr auto kOwner = make_contract_address(240);
  static constexpr auto kSpender = make_contract_address(241);
  static constexpr auto kReceiver = make_contract_address(242);
  static constexpr int32_t kOwnerId = 70;
  static constexpr int32_t kSpenderId = 71;

  void SetUp() override
  {
    NativeTokenAllowanceTest::SetUp();
    for (auto address :
         {kOwner, kSpender, kReceiver, kNativeTokenContractAddress}) {
      setup_grant_storage_token(address, 1'000'000'000);
    }
    for (auto [address, id] :
         {std::pair{kOwner, kOwnerId}, std::pair{kSpender, kSpenderId}}) {
      auto contract = make_native_contract(id);
      write_contract(contract);
      write_deployment(address, contract);
    }
  }

  template<StringLiteral Name>
  static x23::Async<InvokeStatus<void>> call(NativeSyscallContext &ctx,
                                             auto... args)
  {
    auto client =
        ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
    auto result = co_await client.template invoke_opt<Name>(ctx, args...);
    if (!result || !*result || !**result || !(***result).is_ok()) {
      ADD_FAILURE() << "token call failed";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  }

  static x23::Async<InvokeStatus<void>> spend(NativeSyscallContext &ctx)
  {
    co_return co_await call<u8"transfer_from">(ctx, kOwner, kReceiver,
                                               make_amount(10));
  }

  ExecResult run(NativeContract::function_t owner,
                 ContractAddress address = kOwner,
                 NativeContract::function_t spender = spend,
                 NativeContract::function_t reverted_owner = {})
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> owner_fns;
    owner_fns.emplace(0, std::move(owner));
    if (reverted_owner) {
      owner_fns.emplace(1, std::move(reverted_owner));
    }
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        spender_fns;
    spender_fns.emplace(0, std::move(spender));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contracts;
    contracts.emplace(kOwnerId,
                      std::make_unique<NativeContract>(std::move(owner_fns)));
    contracts.emplace(kSpenderId,
                      std::make_unique<NativeContract>(std::move(spender_fns)));
    contracts.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    NativeContext context(std::move(contracts));
    auto tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = address,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(tx, context);
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    auto result = handler.try_commit_transaction()();
    if (result == ExecResult::ExecResultOK) {
      EXPECT_TRUE(
          handler.emit_events(result, ProposedTransaction{.signed_tx = tx})());
    }
    return result;
  }

  static NativeContract::function_t adjust(int128_t delta)
  {
    return
        [delta](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call<u8"adjust_allowance">(ctx, kSpender,
                                                        make_difference(delta));
        };
  }

  static x23::Async<InvokeStatus<void>> del(NativeSyscallContext &ctx)
  {
    co_return co_await call<u8"delete_allowance">(ctx, kSpender);
  }

  // The interface's way to set an absolute allowance: delete, then adjust in
  // the same transaction under the delete's exclusive lock.
  static NativeContract::function_t set_absolute(int128_t amount)
  {
    return [amount](
               NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
      auto result = co_await del(ctx);
      if (!result) {
        co_return result;
      }
      co_return co_await call<u8"adjust_allowance">(ctx, kSpender,
                                                    make_difference(amount));
    };
  }

  auto allowance_item()
  {
    auto data =
        Serializer<SerializationFormat::kTxs>::serialize(kOwner, kSpender);
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = sha256(*data);
    return lookup_storage_item_from_db(
        AddressAndKey{.address = kNativeTokenContractAddress, .key = key});
  }

  void expect_allowance(int128_t expected)
  {
    auto item = allowance_item();
    ASSERT_TRUE(item);
    EXPECT_EQ(from_xdr(item->nnint()), expected);
  }

  void seed_allowance()
  {
    ASSERT_EQ(run(adjust(100)), ExecResult::ExecResultOK);
    advance_block();
    expect_allowance(100);
  }
};

TEST_F(NativeTokenCleanupTest, DeleteRemovesEntryAndMissingDeleteSucceeds)
{
  seed_allowance();
  ASSERT_EQ(run(del), ExecResult::ExecResultOK);
  advance_block();
  EXPECT_FALSE(allowance_item());
  ASSERT_EQ(run(del), ExecResult::ExecResultOK);
  advance_block();
  EXPECT_FALSE(allowance_item());
}

TEST_F(NativeTokenCleanupTest, DeleteAndRegrantUsesZeroAsTheNewBase)
{
  seed_allowance();
  ASSERT_EQ(
      run([](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto result = co_await del(ctx);
        if (!result) {
          co_return result;
        }
        result = co_await del(ctx);
        if (!result) {
          co_return result;
        }
        co_return co_await call<u8"adjust_allowance">(ctx, kSpender,
                                                      make_difference(7));
      }),
      ExecResult::ExecResultOK);
  advance_block();
  expect_allowance(7);
}

TEST_F(NativeTokenCleanupTest, OrdinaryGrantAndSpendCanBothCommit)
{
  seed_allowance();
  ASSERT_EQ(run(adjust(5)), ExecResult::ExecResultOK);
  ASSERT_EQ(run(adjust(0), kSpender), ExecResult::ExecResultOK);
  advance_block();
  expect_allowance(95);
}

enum class AllowanceTxOrder { kExclusiveFirst, kOrdinaryFirst };

class NativeTokenCleanupOrderTest
  : public NativeTokenCleanupTest,
    public ::testing::WithParamInterface<AllowanceTxOrder> {};

TEST_P(NativeTokenCleanupOrderTest, AbsoluteSetConflictsWithSpending)
{
  seed_allowance();
  const bool owner_first = GetParam() == AllowanceTxOrder::kExclusiveFirst;
  const auto first = owner_first ? kOwner : kSpender;
  const auto second = owner_first ? kSpender : kOwner;
  ASSERT_EQ(run(set_absolute(30), first), ExecResult::ExecResultOK);
  EXPECT_EQ(run(set_absolute(30), second),
            ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  expect_allowance(owner_first ? 30 : 90);
}

TEST_P(NativeTokenCleanupOrderTest, DeleteConflictsWithSpending)
{
  seed_allowance();
  const bool owner_first = GetParam() == AllowanceTxOrder::kExclusiveFirst;
  const auto first = owner_first ? kOwner : kSpender;
  const auto second = owner_first ? kSpender : kOwner;
  ASSERT_EQ(run(del, first), ExecResult::ExecResultOK);
  EXPECT_EQ(run(del, second), ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  if (owner_first) {
    EXPECT_FALSE(allowance_item());
  }
  else {
    expect_allowance(90);
  }
}

TEST_P(NativeTokenCleanupOrderTest, DeleteConflictsWithRegrant)
{
  seed_allowance();
  const bool delete_first = GetParam() == AllowanceTxOrder::kExclusiveFirst;
  auto first = delete_first ? NativeContract::function_t(del) : adjust(5);
  auto second = delete_first ? adjust(5) : NativeContract::function_t(del);
  ASSERT_EQ(run(std::move(first)), ExecResult::ExecResultOK);
  EXPECT_EQ(run(std::move(second)),
            ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  if (delete_first) {
    EXPECT_FALSE(allowance_item());
  }
  else {
    expect_allowance(105);
  }
}

INSTANTIATE_TEST_SUITE_P(NativeToken, NativeTokenCleanupOrderTest,
                         ::testing::Values(AllowanceTxOrder::kExclusiveFirst,
                                           AllowanceTxOrder::kOrdinaryFirst),
                         [](auto const &info) {
                           return info.param ==
                                          AllowanceTxOrder::kExclusiveFirst
                                      ? "ExclusiveFirst"
                                      : "OrdinaryFirst";
                         });

TEST_F(NativeTokenCleanupTest, DeleteLockSurvivesOrdinaryRegrantAndNestedSpend)
{
  seed_allowance();
  ASSERT_EQ(
      run([](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto result = co_await del(ctx);
        if (!result) {
          co_return result;
        }
        result = co_await call<u8"adjust_allowance">(ctx, kSpender,
                                                     make_difference(20));
        if (!result) {
          co_return result;
        }
        CopyableInvokeInputs inputs{.entrypoint_id = 0,
                                    .gas_limit = UINT64_MAX,
                                    .flags =
                                        std::to_underlying(InvokeFlag::None)};
        CopyableInvokeOutputs outputs{};
        auto invoke = co_await ctx.syscall<SyscallNo::kInvoke>(
            kSpender, std::span<const std::byte>{},
            std::span<const std::byte>{}, inputs, outputs);
        if (!invoke || *invoke != SyscallErrorCode::kSyscallErrorCodeNone ||
            outputs.frame_status != InvocationFrameStatusCode::Return) {
          ADD_FAILURE() << "nested spend failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      }),
      ExecResult::ExecResultOK);
  EXPECT_EQ(run(adjust(5)), ExecResult::ExecResultTxNonDeterministicError);
  advance_block();
  expect_allowance(10);
}

TEST_F(NativeTokenCleanupTest, RevertedChildDiscardsDeleteAndLock)
{
  seed_allowance();
  auto parent =
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    for (auto [method, expected] :
         {std::pair{1U, InvocationFrameStatusCode::Fault},
          std::pair{0U, InvocationFrameStatusCode::Return}}) {
      CopyableInvokeInputs inputs{.entrypoint_id = method,
                                  .gas_limit = UINT64_MAX,
                                  .flags =
                                      std::to_underlying(InvokeFlag::None)};
      CopyableInvokeOutputs outputs{};
      auto invoke =
          co_await ctx.syscall<SyscallNo::kInvoke>(kOwner,
                                                   std::span<const std::byte>{},
                                                   std::span<const std::byte>{},
                                                   inputs, outputs);
      if (!invoke || *invoke != SyscallErrorCode::kSyscallErrorCodeNone ||
          outputs.frame_status != expected) {
        ADD_FAILURE() << "unexpected child outcome";
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
    }
    co_return InvokeStatus<void>{};
  };
  auto reverted_owner =
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    auto result = co_await del(ctx);
    if (!result) {
      co_return result;
    }
    co_return InvokeError{InvokeErrorCode::Deterministic};
  };
  ASSERT_EQ(run(adjust(5), kSpender, std::move(parent),
                std::move(reverted_owner)),
            ExecResult::ExecResultOK);
  ASSERT_EQ(run(adjust(0), kSpender), ExecResult::ExecResultOK);
  advance_block();
  expect_allowance(95);
}

TEST_F(NativeTokenCleanupTest, DeleteBalanceRejectsWithoutChangingBalance)
{
  ASSERT_EQ(
      run([](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto calldata = ContractTraitMeta<itc_erc20::ERC20>::make_calldata<
            u8"delete_balance">();
        CopyableInvokeInputs inputs{
            .entrypoint_id =
                sdk_method_id(ContractTraitMeta<itc_erc20::ERC20>::entrypoint<
                              u8"delete_balance">()),
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::None)};
        CopyableInvokeOutputs outputs{};
        auto invoke = co_await ctx.syscall<SyscallNo::kInvoke>(
            kNativeTokenContractAddress,
            std::as_bytes(std::span(calldata.data(), calldata.size())),
            std::span<const std::byte>{}, inputs, outputs);
        if (!invoke || *invoke != SyscallErrorCode::kSyscallErrorCodeNone ||
            outputs.frame_status != InvocationFrameStatusCode::Fault) {
          ADD_FAILURE() << "delete_balance did not reject deterministically";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      }),
      ExecResult::ExecResultOK);
  advance_block();
  auto balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kOwner));
  ASSERT_TRUE(balance);
  EXPECT_EQ(from_xdr(balance->nnint()),
            1'000'000'000 - replay_cache_cost_charged(1));
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
