#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

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

  auto owner_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r, _g] =
                 client.invoke_opt<"allowance">(ctx.handler(), kOwnerAddr,
                                                kSpenderAddr);
             if (!r.has_value() || !r.value().has_value() ||
                 !r.value().value().has_value() ||
                 !r.value().value().value().is_ok() ||
                 amount_to_u128(r.value().value().value().ok()) != 0) {
               ADD_FAILURE() << "fresh allowance did not return Ok(0)";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

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

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kOwnerAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
}

// 2. First positive adjust_allowance on a fresh pair succeeds, and the
// subsequent allowance() returns the written value.
TEST_F(NativeTokenAllowanceTest, FirstAdjustAllowanceFromZeroSucceeds)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(172);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(173);

  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  auto owner_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r_adj, _g1] =
                 client.invoke_opt<"adjust_allowance">(ctx.handler(),
                                                       kSpenderAddr,
                                                       make_difference(100));
             if (!r_adj.has_value() || !r_adj.value().has_value() ||
                 !r_adj.value().value().has_value() ||
                 !r_adj.value().value().value().is_ok()) {
               ADD_FAILURE() << "first adjust_allowance(+100) did not succeed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             auto [r_al, _g2] =
                 client.invoke_opt<"allowance">(ctx.handler(), kOwnerAddr,
                                                kSpenderAddr);
             if (!r_al.has_value() || !r_al.value().has_value() ||
                 !r_al.value().value().has_value() ||
                 !r_al.value().value().value().is_ok() ||
                 amount_to_u128(r_al.value().value().value().ok()) != 100) {
               ADD_FAILURE() << "allowance() after +100 adjust was not 100";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

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

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kOwnerAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
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

  auto spender_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r, _g] =
                 client.invoke_opt<"transfer_from">(ctx.handler(), kOwnerAddr,
                                                    kReceiverAddr,
                                                    make_amount(50));
             (void)r;
             return {};
           }}});

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

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kSpenderAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_NE(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// 4. Two adjust_allowance invocations in one transaction accumulate.
TEST_F(NativeTokenAllowanceTest, AllowanceAccumulatesAcrossAdjusts)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(177);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(178);

  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost());

  auto owner_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             for (int128_t d : {100, 50}) {
               auto [r, _g] =
                   client.invoke_opt<"adjust_allowance">(ctx.handler(),
                                                         kSpenderAddr,
                                                         make_difference(d));
               if (!r.has_value() || !r.value().has_value() ||
                   !r.value().value().has_value() ||
                   !r.value().value().value().is_ok()) {
                 ADD_FAILURE() << "adjust_allowance step failed";
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }
             auto [r_al, _g] =
                 client.invoke_opt<"allowance">(ctx.handler(), kOwnerAddr,
                                                kSpenderAddr);
             if (!r_al.has_value() || !r_al.value().has_value() ||
                 !r_al.value().value().has_value() ||
                 !r_al.value().value().value().is_ok() ||
                 amount_to_u128(r_al.value().value().value().ok()) != 150) {
               ADD_FAILURE() << "accumulated allowance was not 150";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

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

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kOwnerAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
}

// 5. transfer_from consumes allowance: owner grants 100, spender transfers 50,
// allowance becomes 50; a second transfer of 60 must fail at commit.
TEST_F(NativeTokenAllowanceTest, TransferFromDeductsFromAllowance)
{
  constexpr static ContractAddress kOwnerAddr = make_contract_address(179);
  constexpr static ContractAddress kSpenderAddr = make_contract_address(180);
  constexpr static ContractAddress kReceiverAddr = make_contract_address(181);

  setup_grant_storage_token(kOwnerAddr, 1'000'000);
  setup_grant_storage_token(
      GenesisConfigurationLoader::kPresetNativeTokenAddress,
      get_default_cost_model()->get_nnint_cost() * 4);

  auto owner_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r, _g] = client.invoke_opt<"adjust_allowance">(
                 ctx.handler(), kSpenderAddr, make_difference(100'000));
             if (!r.has_value() || !r.value().has_value() ||
                 !r.value().value().has_value() ||
                 !r.value().value().value().is_ok()) {
               ADD_FAILURE() << "grant +100000 failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }},
          {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r, _g] =
                 client.invoke_opt<"allowance">(ctx.handler(), kOwnerAddr,
                                                kSpenderAddr);
             if (!r.has_value() || !r.value().has_value() ||
                 !r.value().value().has_value() ||
                 !r.value().value().value().is_ok() ||
                 amount_to_u128(r.value().value().value().ok()) != 50'000) {
               ADD_FAILURE() << "residual allowance was not 50000";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  auto spender_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r, _g] =
                 client.invoke_opt<"transfer_from">(ctx.handler(), kOwnerAddr,
                                                    kReceiverAddr,
                                                    make_amount(50'000));
             if (!r.has_value() || !r.value().has_value() ||
                 !r.value().value().has_value() ||
                 !r.value().value().value().is_ok()) {
               ADD_FAILURE() << "transfer_from(50000) did not return Ok";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }},
          {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             auto [r, _g] =
                 client.invoke_opt<"transfer_from">(ctx.handler(), kOwnerAddr,
                                                    kReceiverAddr,
                                                    make_amount(60'000));
             (void)r;
             return {};
           }}});

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
    auto h = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = kOwnerAddr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        *runtime_context);
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK)());
  }

  advance_block();

  // Tx 2: spender transfers 50000.
  {
    auto h = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = kSpenderAddr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        *runtime_context);
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK)());
  }

  advance_block();

  // Tx 3: owner asserts allowance == 50000.
  {
    auto h = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = kOwnerAddr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(1),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        *runtime_context);
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK)());
  }

  advance_block();

  // Tx 4: spender tries transfer_from(60000), must fail at commit.
  {
    auto h = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = kSpenderAddr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(1),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        *runtime_context);
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

  auto owner_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto client = ContractTraitClient<itc_erc20::ERC20>(
                 GenesisConfigurationLoader::kPresetNativeTokenAddress);
             for (int128_t d : {100, -150}) {
               auto [r, _g] =
                   client.invoke_opt<"adjust_allowance">(ctx.handler(),
                                                         kSpenderAddr,
                                                         make_difference(d));
               (void)r;
             }
             return {};
           }}});

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

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kOwnerAddr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *runtime_context);

  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_NE(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
