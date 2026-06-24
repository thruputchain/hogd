#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/utilities.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "standard_asset_events.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class NativeTokenAllowanceTest : public ::testing::Test,
                                 public NativeContractTestFramework {
public:
  NativeTokenAllowanceTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    auto r = client.invoke_opt<"allowance">(ctx, kOwnerAddr, kSpenderAddr);
    if (!r.has_value() || !r.value().has_value() ||
        !r.value().value().has_value() || !r.value().value().value().is_ok() ||
        amount_to_u128(r.value().value().value().ok()) != 0) {
      ADD_FAILURE() << "fresh allowance did not return Ok(0)";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    auto r_adj = client.invoke_opt<"adjust_allowance">(ctx, kSpenderAddr,
                                                       make_difference(100));
    if (!r_adj.has_value() || !r_adj.value().has_value() ||
        !r_adj.value().value().has_value() ||
        !r_adj.value().value().value().is_ok()) {
      ADD_FAILURE() << "first adjust_allowance(+100) did not succeed";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    auto r_al = client.invoke_opt<"allowance">(ctx, kOwnerAddr, kSpenderAddr);
    if (!r_al.has_value() || !r_al.value().has_value() ||
        !r_al.value().value().has_value() ||
        !r_al.value().value().value().is_ok() ||
        amount_to_u128(r_al.value().value().value().ok()) != 100) {
      ADD_FAILURE() << "allowance() after +100 adjust was not 100";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    auto r = client.invoke_opt<"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                                make_amount(50));
    (void)r;
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_NE(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    for (int128_t d : {100, 50}) {
      auto r = client.invoke_opt<"adjust_allowance">(ctx, kSpenderAddr,
                                                     make_difference(d));
      if (!r.has_value() || !r.value().has_value() ||
          !r.value().value().has_value() ||
          !r.value().value().value().is_ok()) {
        ADD_FAILURE() << "adjust_allowance step failed";
        return InvokeError{InvokeErrorCode::Deterministic};
      }
    }
    auto r_al = client.invoke_opt<"allowance">(ctx, kOwnerAddr, kSpenderAddr);
    if (!r_al.has_value() || !r_al.value().has_value() ||
        !r_al.value().value().has_value() ||
        !r_al.value().value().value().is_ok() ||
        amount_to_u128(r_al.value().value().value().ok()) != 150) {
      ADD_FAILURE() << "accumulated allowance was not 150";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r =
            client.invoke_opt<"adjust_allowance">(ctx, kSpenderAddr,
                                                  make_difference(100'000));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "grant +100000 failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_owner.emplace(
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = client.invoke_opt<"allowance">(ctx, kOwnerAddr, kSpenderAddr);
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok() ||
            amount_to_u128(r.value().value().value().ok()) != 50'000) {
          ADD_FAILURE() << "residual allowance was not 50000";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r =
            client.invoke_opt<"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                               make_amount(50'000));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer_from(50000) did not return Ok";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_spender.emplace(
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r =
            client.invoke_opt<"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                               make_amount(60'000));
        (void)r;
        return {};
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_NE(h.try_commit_transaction(), ExecResult::ExecResultOK);
  }
}

// 6. A net-negative sequence of adjust_allowance calls fails at commit.
TEST_F(NativeTokenAllowanceTest, NegativeAdjustCannotDriveAllowanceBelowZero)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(182);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(183);

  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    for (int128_t d : {100, -150}) {
      auto r = client.invoke_opt<"adjust_allowance">(ctx, kSpenderAddr,
                                                     make_difference(d));
      (void)r;
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_NE(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    auto r = client.invoke_opt<"transfer">(ctx, kOwnerAddr, make_amount(37));
    if (!r.has_value() || !r.value().has_value() ||
        !r.value().value().has_value() || !r.value().value().value().is_ok()) {
      ADD_FAILURE() << "self-transfer did not return Ok";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
// aborted immediately and leaves the prior allowance intact. (Balance, unlike
// allowance, stays deferred -- see erc20_parity_test.cc for that gap.)
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
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = client.invoke_opt<"adjust_allowance">(ctx, kSpenderAddr,
                                                       make_difference(
                                                           kSeedAllowance));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "seed adjust_allowance failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns_owner.emplace(
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r_adj = client.invoke_opt<"adjust_allowance">(
            ctx, kSpenderAddr, make_difference(kOverflowIncrement));
        // Strict allowance must reject the overflowing increment at invoke.
        if (r_adj.has_value() && r_adj.value().has_value() &&
            r_adj.value().value().has_value() &&
            r_adj.value().value().value().is_ok()) {
          ADD_FAILURE()
              << "overflowing adjust_allowance(+10) should be rejected";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  // Spender method 0: transfer_from kTransferAmount. The allowance is still the
  // seeded INT128_MAX - 5 (the +10 adjust aborted), so this succeeds.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r_xfer =
            client.invoke_opt<"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                               make_amount(kTransferAmount));
        if (!r_xfer.has_value() || !r_xfer.value().has_value() ||
            !r_xfer.value().value().has_value() ||
            !r_xfer.value().value().value().is_ok()) {
          ADD_FAILURE() << "transfer_from against seeded allowance should "
                           "succeed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    auto r = client.invoke_opt<"adjust_allowance">(ctx, kOwnerAddr,
                                                   make_difference(100));
    if (r.has_value() && r.value().has_value() &&
        r.value().value().has_value() && r.value().value().value().is_ok()) {
      ADD_FAILURE() << "adjust_allowance for self should not succeed";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto client = ContractTraitClient<itc_erc20::ERC20>(
        GenesisConfigurationLoader::kPresetNativeTokenAddress);
    auto r = client.invoke_opt<"transfer_from">(ctx, kOwnerAddr, kReceiverAddr,
                                                make_amount(100));
    if (!r.has_value() || !r.value().has_value() ||
        !r.value().value().has_value() || !r.value().value().value().is_ok()) {
      ADD_FAILURE() << "self transfer_from should succeed";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
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

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto delta = make_difference(kTransferAmount);
        auto r =
            client.invoke_opt<"adjust_allowance">(ctx, kSpenderAddr, delta);
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "owner allowance grant failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  // Method 1: after the zero-recipient transfer_from, the allowance is fully
  // consumed and total_supply() is still the fixed native HOG supply.
  fns_owner.emplace(
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);

        auto allowance =
            client.invoke_opt<"allowance">(ctx, kOwnerAddr, kSpenderAddr);
        if (!allowance.has_value() || !allowance.value().has_value() ||
            !allowance.value().value().has_value() ||
            !allowance.value().value().value().is_ok() ||
            amount_to_u128(allowance.value().value().value().ok()) != 0) {
          ADD_FAILURE() << "residual allowance was not consumed to zero";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto const expected_total_supply = make_amount(
            uint128_t{kTotalHogSupply} * uint128_t{kHogDivisibility});
        auto total_supply = client.invoke_opt<"total_supply">(ctx);
        if (!total_supply.has_value() || !total_supply.value().has_value() ||
            !total_supply.value().value().has_value() ||
            !total_supply.value().value().value().is_ok() ||
            total_supply.value().value().value().ok() !=
                expected_total_supply) {
          ADD_FAILURE() << "total_supply changed after zero-recipient transfer";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto owner_contract = std::make_unique<NativeContract>(std::move(fns_owner));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_spender;
  // Method 0: spender transfers the owner's funds to the all-zero address.
  fns_spender.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto client = ContractTraitClient<itc_erc20::ERC20>(
            GenesisConfigurationLoader::kPresetNativeTokenAddress);
        auto r = client.invoke_opt<"transfer_from">(
            ctx, kOwnerAddr, kZeroAddr,
            make_amount(static_cast<uint128_t>(kTransferAmount)));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE()
              << "transfer_from(owner, zero, amount) did not return Ok";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
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
  ASSERT_EQ(event.serialized_event.size(), expected_event.size());
  EXPECT_TRUE(std::equal(event.serialized_event.begin(),
                         event.serialized_event.end(), expected_event.begin()));

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
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
