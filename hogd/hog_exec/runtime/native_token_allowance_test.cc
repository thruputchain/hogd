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
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
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

  static uint128_t amount_to_u128(std::tuple<uint64_t, uint64_t> const &a)
  {
    return (uint128_t{std::get<0>(a)} << 64) | uint128_t{std::get<1>(a)};
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
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r_adj = co_await
            client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                    make_difference(100));
        if (!r_adj.has_value() || !r_adj.value().has_value() ||
            !r_adj.value().value().has_value() ||
            !r_adj.value().value().value().is_ok()) {
          ADD_FAILURE() << "first adjust_allowance(+100) did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto r_al = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) noexcept
                  -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    GenesisConfigurationLoader::kPresetNativeTokenAddress);
                auto r = co_await client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr,
                                                              kReceiverAddr,
                                                              make_amount(50));
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        for (int128_t d : {100, 50}) {
          auto r = co_await client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                           make_difference(d));
          if (!r.has_value() || !r.value().has_value() ||
              !r.value().value().has_value() ||
              !r.value().value().value().is_ok()) {
            ADD_FAILURE() << "adjust_allowance step failed";
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        auto r_al = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost() * 4);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  fns_owner.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await
            client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                    make_difference(100'000));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "grant +100000 failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns_owner.emplace(
      1,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
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
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await
            client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
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
      1,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await
            client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);

        auto grant = co_await
            client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                    make_difference(kGrant));
        if (!grant.has_value() || !grant.value().has_value() ||
            !grant.value().value().has_value() ||
            !grant.value().value().value().is_ok()) {
          ADD_FAILURE() << "initial allowance grant did not co_return Ok";
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
          ADD_FAILURE() << "a decrement past the running allowance must be "
                           "rejected at invoke, not deferred to commit";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // The rejected decrement staged nothing: the grant is still visible.
        auto allowance = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
        if (!allowance.has_value() || !allowance.value().has_value() ||
            !allowance.value().value().has_value() ||
            !allowance.value().value().value().is_ok() ||
            amount_to_u128(allowance.value().value().value().ok()) !=
                static_cast<uint128_t>(kGrant)) {
          ADD_FAILURE() << "allowance after the rejected decrement is not the "
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
      AddressAndKey{.address =
                        GenesisConfigurationLoader::kPresetNativeTokenAddress,
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
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await
            client.invoke_opt<u8"transfer">(ctx, kOwnerAddr, make_amount(37));
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost() * 4);

  // Owner method 0: seed allowance for spender.
  // Owner method 1: adjust allowance for spender by +10 (must abort: overflow).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  fns_owner.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                         make_difference(
                                                             kSeedAllowance));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "seed adjust_allowance failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns_owner.emplace(
      1,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
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
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r_xfer = co_await
            client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                                 make_amount(kTransferAmount));
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

// 8. adjust_allowance for self (owner == spender) must fail.
TEST_F(NativeTokenAllowanceTest, AdjustAllowanceForSelfFails)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(196);

  // kOwnerAddr is the root invoked address and commits 1 tx; grant it enough
  // to pay the replay-cache storage cost.
  setup_grant_storage_token(kOwnerAddr, replay_cache_cost_charged(1));
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await client.invoke_opt<u8"adjust_allowance">(ctx, kOwnerAddr,
                                                         make_difference(100));
        if (r.has_value() && r.value().has_value() &&
            r.value().value().has_value() &&
            r.value().value().value().is_ok()) {
          ADD_FAILURE() << "adjust_allowance for self should not succeed";
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress, 0);

  auto allowance_key_data =
      Serializer<SerializationFormat::kTxs>::serialize(kOwnerAddr, kOwnerAddr);
  ASSERT_TRUE(allowance_key_data.has_value());
  StorageKey allowance_storage_key(StorageKeyType::ContractData);
  allowance_storage_key.contract_data_key() = sha256(*allowance_key_data);
  AddressAndKey allowance_addrkey{
      .address = GenesisConfigurationLoader::kPresetNativeTokenAddress,
      .key = std::move(allowance_storage_key)};
  ASSERT_FALSE(lookup_storage_item_from_db(allowance_addrkey).has_value());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) noexcept
                  -> x23::Async<InvokeStatus<void>> {
                auto client = ContractTraitClient<itc_erc20::ERC20>(
                    GenesisConfigurationLoader::kPresetNativeTokenAddress);
                auto r = co_await client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr,
                                                              kReceiverAddr,
                                                              make_amount(100));
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

  auto native_token_balance_item =
      lookup_storage_item_from_db(make_free_native_token_balance_addrkey(
          GenesisConfigurationLoader::kPresetNativeTokenAddress));
  ASSERT_TRUE(native_token_balance_item.has_value());
  EXPECT_EQ(from_xdr(native_token_balance_item->nnint()), 0)
      << "native token contract storage balance must be unchanged";
}

// Characterization test (issue #2135, amendment #005): transfer_from(owner,
// ContractAddress{}, amount) by an allowance-authorized spender is accepted
// just like any other delegated transfer. The spender's allowance is consumed,
// the owner is debited, and the all-zero address is credited as an ordinary
// native-token balance row. Because the native token has no burn semantics
// (burn() is disabled and total_supply() is a fixed constant), the zero
// recipient is NOT special-cased as a supply-reducing burn: total_supply()
// stays fixed and a standard Transfer(owner, zero, amount) event is emitted.
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      cost_model->get_nnint_cost() * 4);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  // Method 0: owner grants the spender an allowance for the transfer.
  fns_owner.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto delta = make_difference(kTransferAmount);
        auto r = co_await
            client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr, delta);
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "owner allowance grant failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  // Method 1: after the zero-recipient transfer_from, the allowance is fully
  // consumed and total_supply() is still the fixed native HOG supply.
  fns_owner.emplace(
      1,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);

        auto allowance = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
        if (!allowance.has_value() || !allowance.value().has_value() ||
            !allowance.value().value().has_value() ||
            !allowance.value().value().value().is_ok() ||
            amount_to_u128(allowance.value().value().value().ok()) != 0) {
          ADD_FAILURE() << "residual allowance was not consumed to zero";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto const expected_total_supply = make_amount(
            uint128_t{kTotalHogSupply} * uint128_t{kHogDivisibility});
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
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kZeroAddr,
            make_amount(static_cast<uint128_t>(kTransferAmount)));
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
  EXPECT_EQ(event.contract_address,
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  // Owner method 0: grant the spender an allowance of kGrantAllowance.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_owner;
  fns_owner.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await client.invoke_opt<u8"adjust_allowance">(ctx, kSpenderAddr,
                                                         make_difference(
                                                             kGrantAllowance));
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
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr,
            make_amount(static_cast<uint128_t>(kTransferAmount)));
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
  AddressAndKey allowance_addrkey{
      .address = GenesisConfigurationLoader::kPresetNativeTokenAddress,
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      cost_model->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> spender_fns;
  spender_fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);

        auto first = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr,
            make_amount(static_cast<uint128_t>(kTransferAmount)));
        if (!first.has_value() || !first.value().has_value() ||
            !first.value().value().has_value() ||
            !first.value().value().value().is_ok()) {
          ADD_FAILURE() << "same-tx transfer_from did not consume the grant";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto second = co_await client.invoke_opt<u8"transfer_from">(
            ctx, kOwnerAddr, kReceiverAddr,
            make_amount(static_cast<uint128_t>(kSecondTransferAmount)));
        if (!second.has_value() || !second.value().has_value() ||
            !second.value().value().has_value() ||
            second.value().value().value().is_ok()) {
          ADD_FAILURE() << "second transfer_from did not abort on the "
                           "remaining allowance";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto allowance = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
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
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
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
  AddressAndKey allowance_addrkey{
      .address = GenesisConfigurationLoader::kPresetNativeTokenAddress,
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
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto transfer = co_await
            client.invoke_opt<u8"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                                 make_amount(0));
        if (!transfer.has_value() || !transfer.value().has_value() ||
            !transfer.value().value().has_value() ||
            !transfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "zero transfer_from over fresh keys failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto allowance = co_await
            client.invoke_opt<u8"allowance">(ctx, kOwnerAddr, kSpenderAddr);
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
  EXPECT_FALSE(
      lookup_storage_item_from_db(
          AddressAndKey{
              .address = GenesisConfigurationLoader::kPresetNativeTokenAddress,
              .key = std::move(allowance_storage_key)})
          .has_value());
}

TEST_F(NativeTokenAllowanceTest,
       GenesisRootBurnAuthorityIsNotHeightLimitedByPrecompile)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(236);
  constexpr static int128_t kInitialSupply = 1'000;
  constexpr static int128_t kBurnAmount = 400;

  setup_grant_storage_token(kGenesisRootContractAddress,
                            replay_cache_cost_charged(1));
  setup_grant_storage_token(kOwnerAddr, kInitialSupply);

  StorageKey supply_key(StorageKeyType::ContractData);
  supply_key.contract_data_key() = kNativeTokenTotalSupplyKey;
  const AddressAndKey supply_addrkey{.address = kNativeTokenContractAddress,
                                     .key = supply_key};
  write_item(supply_addrkey, std::make_shared<const StorageItemWrapper>(
                                 kInitialSupply, get_default_cost_model()));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto burn = co_await client.invoke_opt<u8"burn">(ctx, kOwnerAddr,
                                                make_amount(kBurnAmount));
        if (!burn.has_value() || !burn.value().has_value() ||
            !burn.value().value().has_value() ||
            !burn.value().value().value().is_ok()) {
          ADD_FAILURE() << "genesis-root burn did not succeed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns.emplace(
      1,
      [](NativeSyscallContext &ctx) noexcept -> x23::Async<InvokeStatus<void>> {
        auto client =
            ContractTraitClient<itc_erc20::ERC20>(kNativeTokenContractAddress);
        auto burn = co_await
            client.invoke_opt<u8"burn">(ctx, kOwnerAddr, make_amount(1));
        if (!burn.has_value() || !burn.value().has_value() ||
            !burn.value().value().has_value() ||
            !burn.value().value().value().is_ok()) {
          ADD_FAILURE() << "genesis-root burn failed after height 1";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto root_contract = std::make_unique<NativeContract>(std::move(fns));

  constexpr int32_t kRootNativeId = 43;
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kRootNativeId, std::move(root_contract));
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto root_xdr = make_native_contract(kRootNativeId);
  write_contract(root_xdr);
  write_deployment(kGenesisRootContractAddress, root_xdr);

  auto handler_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address =
                                         kGenesisRootContractAddress,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  EphemeralTxIdentifier tx_id;
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());
  advance_block();

  auto owner_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kOwnerAddr));
  ASSERT_TRUE(owner_balance.has_value());
  EXPECT_EQ(from_xdr(owner_balance->nnint()), kInitialSupply - kBurnAmount);

  auto total_supply = lookup_storage_item_from_db(supply_addrkey);
  ASSERT_TRUE(total_supply.has_value());
  EXPECT_EQ(from_xdr(total_supply->nnint()), kInitialSupply - kBurnAmount);

  auto event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(event_log.has_value());
  ASSERT_EQ(event_log->exec_result, ExecResult::ExecResultOK);
  ASSERT_EQ(event_log->events.size(), 1U);
  auto const &event = event_log->events.front();
  EXPECT_EQ(event.contract_address, kNativeTokenContractAddress);
  EXPECT_EQ(event.num_topics, 3U);

  ContractAddress zero{};
  auto expected_event =
      make_event_from_cxs_struct(native_token_transfer_event_signature(),
                                 TransferEvent{
                                     .from_address = kOwnerAddr,
                                     .to_address = zero,
                                     .amount = to_xdr(kBurnAmount),
                                 });
  ASSERT_TRUE(expected_event);
  EXPECT_TRUE(std::ranges::equal(event.serialized_event, *expected_event));

  // The precompile continues to trust the root after the ceremony. Issuance
  // policy, including any height restriction, belongs to the root contract.
  setup_grant_storage_token(kGenesisRootContractAddress,
                            replay_cache_cost_charged(1));
  auto late_tx = SignedTransaction{
      .tx =
          Transaction{.invocation =
                          Invocation{.invoked_address =
                                         kGenesisRootContractAddress,
                                     .entrypoint = entrypoint_from_method_id(1),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
                      .uniquifier = make_new_uniquifier()}};
  auto late_handler = make_new_system_call_handler(late_tx, *runtime_context);
  ASSERT_TRUE(late_handler.root_invoke()().is_root_success());
  ASSERT_EQ(late_handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      late_handler.emit_events(ExecResult::ExecResultOK,
                               ProposedTransaction{.signed_tx = late_tx})());
  advance_block();

  owner_balance = lookup_storage_item_from_db(
      make_free_native_token_balance_addrkey(kOwnerAddr));
  ASSERT_TRUE(owner_balance.has_value());
  EXPECT_EQ(from_xdr(owner_balance->nnint()), kInitialSupply - kBurnAmount - 1);
  total_supply = lookup_storage_item_from_db(supply_addrkey);
  ASSERT_TRUE(total_supply.has_value());
  EXPECT_EQ(from_xdr(total_supply->nnint()), kInitialSupply - kBurnAmount - 1);
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

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
