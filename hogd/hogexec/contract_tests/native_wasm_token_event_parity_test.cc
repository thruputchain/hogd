#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

//
// Side-by-side event-parity test: runs the canonical wasm `itc_erc20` contract
// and the precompiled native token contract through the same transfer/allowance
// operations and asserts the emitted events are byte-identical, modulo the
// emitting contract address.
//
// The two implementations share the `itc_erc20::ERC20` trait (and therefore the
// same method IDs), so a single native wrapper contract can drive either one by
// taking the target token address as a calldata argument.
//

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class NativeWasmTokenEventParityTest : public ::testing::Test,
                                       public NativeContractTestFramework {
public:
  NativeWasmTokenEventParityTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  // Actor addresses, shared identically across both the wasm and native runs so
  // that the only legitimate difference between emitted events is the emitting
  // contract address.
  constexpr static ContractAddress kAlice = make_contract_address(201);
  constexpr static ContractAddress kBob = make_contract_address(202);
  constexpr static ContractAddress kSpender = make_contract_address(203);
  constexpr static ContractAddress kWasmToken = make_contract_address(204);

  // Amounts used by the wrapper methods (event payloads must match across
  // runs). The transfer amounts must exceed the native token's one-time
  // balance-creation cost (deducted from the recipient's credited balance, so a
  // tiny transfer would underflow and abort the commit non-deterministically).
  constexpr static uint128_t kMintAmount = 10'000'000;
  constexpr static uint128_t kTransferAmount = 500'000;
  constexpr static int128_t kAllowanceAmount = 400'000;
  constexpr static uint128_t kTransferFromAmount = 200'000;

  // Wrapper method IDs.
  constexpr static Runtime::MethodID kMethodTransfer = 0;
  constexpr static Runtime::MethodID kMethodAdjustAllowance = 1;
  constexpr static Runtime::MethodID kMethodTransferFrom = 2;
  constexpr static Runtime::MethodID kMethodInitialize = 3;
  constexpr static Runtime::MethodID kMethodMint = 4;

  constexpr static int32_t kWrapperNativeId = 40;

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  static std::tuple<int64_t, uint64_t> make_difference(int128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  // Invoke `Name` on the ERC20 contract deployed at `target` and require Ok.
  template<StringLiteral Name, typename... Args>
  static InvokeStatus<void> call_ok(NativeSyscallContext &ctx,
                                    ContractAddress target, Args... args)
  {
    auto client = ContractTraitClient<itc_erc20::ERC20>(target);
    auto r = client.template invoke_opt<Name>(ctx, args...);
    if (!r.has_value() || !r.value().has_value() ||
        !r.value().value().has_value() || !r.value().value().value().is_ok()) {
      ADD_FAILURE() << "itc_erc20 call did not return Ok: "
                    << std::string_view(reinterpret_cast<const char *>(
                                            Name.get().data()),
                                        Name.get().size());
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  }

protected:
  void SetUp() override
  {
    // Deploy the canonical wasm itc_erc20 contract.
    using meta = ContractTraitMeta<itc_erc20::ERC20>;
    auto wasm_contract = meta::load_wasm();
    write_contract(wasm_contract);
    write_deployment(kWasmToken, wasm_contract);
    setup_grant_storage_token(kWasmToken, 1'000'000'000);

    // Deploy the precompiled native token at the preset address.
    auto token_contract = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(token_contract);
    write_deployment(GenesisConfigurationLoader::kPresetNativeTokenAddress,
                     token_contract);
    // Cover the native token's own storage writes (e.g. the allowance row).
    setup_grant_storage_token(
        GenesisConfigurationLoader::kPresetNativeTokenAddress, 10'000'000);

    // Build the wrapper contract. Each method takes the target token address as
    // its sole calldata argument and drives the shared ERC20 trait.
    NativeContractBuilder builder;
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodTransfer,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> InvokeStatus<void> {
          return call_ok<u8"transfer">(ctx, target, kBob,
                                       make_amount(kTransferAmount));
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodAdjustAllowance,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> InvokeStatus<void> {
          return call_ok<u8"adjust_allowance">(ctx, target, kSpender,
                                               make_difference(
                                                   kAllowanceAmount));
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodTransferFrom,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> InvokeStatus<void> {
          return call_ok<u8"transfer_from">(ctx, target, kAlice, kBob,
                                            make_amount(kTransferFromAmount));
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodInitialize,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> InvokeStatus<void> {
          return call_ok<u8"initialize">(ctx, target, kAlice);
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodMint,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> InvokeStatus<void> {
          return call_ok<u8"mint">(ctx, target, kAlice,
                                   make_amount(kMintAmount));
        });
    auto wrapper_contract = builder.build();

    // Native runtime context: the wrapper plus the native token precompile.
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(kWrapperNativeId, std::move(wrapper_contract));
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    auto native_context =
        std::make_unique<NativeContext>(std::move(native_map));

    // Multiplex the native engine with the wasmi engine so the wrapper (native)
    // can cross-call the wasm token.
    auto wasm_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
    multiplexed_context_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kNative, {native_context.release(), true}},
            {ContractType::kWasmi, {wasm_context.release(), true}}});

    // Deploy the wrapper at both the Alice and Spender addresses so that the
    // msg_sender seen by the token equals the acting party.
    auto wrapper_xdr = make_native_contract(kWrapperNativeId);
    write_contract(wrapper_xdr);
    write_deployment(kAlice, wrapper_xdr);
    write_deployment(kSpender, wrapper_xdr);

    // Fund Alice (native balance for the native-token transfers + storage +
    // replay cache for the txs she roots) and Spender (replay cache).
    setup_grant_storage_token(kAlice, 50'000'000);
    setup_grant_storage_token(kSpender, 10'000'000);
  }

  // Calldata carrying just the target token address.
  static Calldata target_calldata(ContractAddress target)
  {
    auto buf = Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
        serialize(std::make_tuple(target));
    EXPECT_TRUE(buf.has_value());
    return buf.value();
  }

  // Run a wrapper method that commits but emits no event of interest (setup).
  void run_setup(Runtime::MethodID method, ContractAddress invoked,
                 ContractAddress target)
  {
    SignedTransaction h_tx{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = invoked,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = target_calldata(target)},
                          .uniquifier = make_new_uniquifier()}};
    auto h = make_new_system_call_handler(h_tx.tx, *multiplexed_context_);
    ASSERT_TRUE(h.root_invoke().is_root_success());
    ASSERT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
    advance_block();
  }

  // Run a wrapper method and return the single event it emits.
  std::optional<TxEmittedEvent> run_op(Runtime::MethodID method,
                                       ContractAddress invoked,
                                       ContractAddress target)
  {
    EphemeralTxIdentifier tx_id;
    SignedTransaction h_tx{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = invoked,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = target_calldata(target)},
                          .uniquifier = make_new_uniquifier()}};
    auto h =
        make_new_system_call_handler(h_tx.tx, *multiplexed_context_, &tx_id);
    EXPECT_TRUE(h.root_invoke().is_root_success())
        << "method=" << method << " target0=" << static_cast<int>(target[0]);
    EXPECT_EQ(h.try_commit_transaction(), ExecResult::ExecResultOK)
        << "method=" << method << " target0=" << static_cast<int>(target[0]);
    EXPECT_TRUE(h.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = h_tx})());
    advance_block();

    auto log = get_tx_event_log(tx_id)();
    if (!log.has_value() || log.value().events.size() != 1) {
      ADD_FAILURE() << "expected exactly one emitted event";
      return std::nullopt;
    }
    return log.value().events[0];
  }

  // Assert two events are identical except for the emitting contract address.
  static void
  expect_events_match(std::optional<TxEmittedEvent> const &wasm_ev,
                      std::optional<TxEmittedEvent> const &native_ev,
                      char const *label)
  {
    ASSERT_TRUE(wasm_ev.has_value()) << label;
    ASSERT_TRUE(native_ev.has_value()) << label;
    EXPECT_EQ(wasm_ev->num_topics, 3U) << label;
    EXPECT_EQ(wasm_ev->num_topics, native_ev->num_topics) << label;
    ASSERT_EQ(wasm_ev->serialized_event.size(),
              native_ev->serialized_event.size())
        << label;
    EXPECT_TRUE(std::equal(wasm_ev->serialized_event.begin(),
                           wasm_ev->serialized_event.end(),
                           native_ev->serialized_event.begin()))
        << label << ": serialized event bytes differ (topics + data)";
  }

  std::unique_ptr<MultiplexedContext> multiplexed_context_;
};

TEST_F(NativeWasmTokenEventParityTest,
       TransferAllowanceEventsMatchWasmAndNative)
{
  // --- wasm itc_erc20: initialize + mint to Alice, then the three ops. ---
  run_setup(kMethodInitialize, kAlice, kWasmToken);
  run_setup(kMethodMint, kAlice, kWasmToken);
  auto wasm_transfer = run_op(kMethodTransfer, kAlice, kWasmToken);
  auto wasm_approval = run_op(kMethodAdjustAllowance, kAlice, kWasmToken);
  auto wasm_transfer_from = run_op(kMethodTransferFrom, kSpender, kWasmToken);

  // --- native token: balance was granted in SetUp, so just the three ops. ---
  ContractAddress const native_token =
      GenesisConfigurationLoader::kPresetNativeTokenAddress;
  auto native_transfer = run_op(kMethodTransfer, kAlice, native_token);
  auto native_approval = run_op(kMethodAdjustAllowance, kAlice, native_token);
  auto native_transfer_from =
      run_op(kMethodTransferFrom, kSpender, native_token);

  // Sanity: the events really were emitted by the two different contracts.
  ASSERT_TRUE(wasm_transfer.has_value());
  ASSERT_TRUE(native_transfer.has_value());
  EXPECT_EQ(wasm_transfer->contract_address, kWasmToken);
  EXPECT_EQ(native_transfer->contract_address, native_token);

  expect_events_match(wasm_transfer, native_transfer, "transfer");
  expect_events_match(wasm_approval, native_approval, "adjust_allowance");
  expect_events_match(wasm_transfer_from, native_transfer_from,
                      "transfer_from");
}

// NOLINTEND(bugprone-unchecked-optional-access)
// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
