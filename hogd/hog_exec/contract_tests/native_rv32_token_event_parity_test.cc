#include <gtest/gtest.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/itc_erc20.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

//
// Side-by-side event-parity test: runs the canonical rv32 `itc_erc20` contract
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
class NativeRv32TokenEventParityTest : public ::testing::Test,
                                       public NativeContractTestFramework {
public:
  NativeRv32TokenEventParityTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  // Actor addresses, shared identically across both the rv32 and native runs so
  // that the only legitimate difference between emitted events is the emitting
  // contract address.
  constexpr static ContractAddress kAlice = make_contract_address(201);
  constexpr static ContractAddress kBob = make_contract_address(202);
  constexpr static ContractAddress kSpender = make_contract_address(203);
  constexpr static ContractAddress kRv32Token = make_contract_address(204);

  // Amounts used by the wrapper methods (event payloads must match across
  // runs). The transfer amounts must exceed the native token's one-time
  // balance-creation cost (deducted from the recipient's credited balance, so a
  // tiny transfer would underflow and abort the commit non-deterministically).
  constexpr static uint128_t kMintAmount = 10'000'000;
  constexpr static uint128_t kTransferAmount = 500'000;
  constexpr static int128_t kAllowanceAmount = 400'000;
  constexpr static uint128_t kTransferFromAmount = 200'000;
  constexpr static uint128_t kBurnAmount = 300'000;

  // Wrapper method IDs.
  constexpr static Runtime::MethodID kMethodTransfer = 0;
  constexpr static Runtime::MethodID kMethodAdjustAllowance = 1;
  constexpr static Runtime::MethodID kMethodTransferFrom = 2;
  constexpr static Runtime::MethodID kMethodInitialize = 3;
  constexpr static Runtime::MethodID kMethodMint = 4;
  constexpr static Runtime::MethodID kMethodNettedNegativeAllowanceChange = 5;
  constexpr static Runtime::MethodID kMethodInitializeGenesisRoot = 6;
  constexpr static Runtime::MethodID kMethodBurn = 7;
  constexpr static Runtime::MethodID kMethodClearAllowance = 8;

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
  static x23::Async<InvokeStatus<void>>
  call_ok(NativeSyscallContext &ctx, ContractAddress target, Args... args)
  {
    auto client = ContractTraitClient<itc_erc20::ERC20>(target);
    auto r = co_await client.template invoke_opt<Name>(ctx, args...);
    if (!r.has_value() || !r.value().has_value() ||
        !r.value().value().has_value() || !r.value().value().value().is_ok()) {
      ADD_FAILURE() << "itc_erc20 call did not return Ok: "
                    << std::string_view(reinterpret_cast<const char *>(
                                            Name.get().data()),
                                        Name.get().size());
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  }

  // Invoke `Name` on the ERC20 contract deployed at `target` and require a
  // contract-level rejection carrying exactly `expected`: the child frame
  // must abort with that itc_erc20 error, rather than succeeding, faulting,
  // or reporting some other error. A library exception or an abort payload
  // that does not parse as an itc_erc20 exception lands in a different
  // ContractErrorType alternative and is rejected here too.
  template<StringLiteral Name, typename... Args>
  static x23::Async<InvokeStatus<void>>
  call_expect_err(NativeSyscallContext &ctx, ContractAddress target,
                  itc_erc20::ERC20Error expected, Args... args)
  {
    auto name_str =
        std::string_view(reinterpret_cast<const char *>(Name.get().data()),
                         Name.get().size());
    auto client = ContractTraitClient<itc_erc20::ERC20>(target);
    auto r = co_await client.template invoke_opt<Name>(ctx, args...);
    if (!r.has_value() || !r.value().has_value() ||
        !r.value().value().has_value()) {
      ADD_FAILURE() << "itc_erc20 call faulted instead of returning an ERC20 "
                       "error: "
                    << name_str;
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    auto const &result = r.value().value().value();
    if (result.is_ok()) {
      ADD_FAILURE() << "itc_erc20 call unexpectedly returned Ok: " << name_str;
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    auto reported = std::visit(
        [](auto const &exception) -> std::optional<itc_erc20::ERC20Error> {
          if constexpr (requires {
                          {
                            exception.exn
                          } -> std::convertible_to<itc_erc20::ERC20Error>;
                        }) {
            return exception.exn;
          }
          else {
            return std::nullopt;
          }
        },
        result.err().variant_);
    if (!reported.has_value()) {
      ADD_FAILURE() << "itc_erc20 call aborted without a decodable itc_erc20 "
                       "exception: "
                    << name_str;
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (*reported != expected) {
      ADD_FAILURE() << "itc_erc20 call reported error "
                    << static_cast<int>(*reported) << ", expected "
                    << static_cast<int>(expected) << ": " << name_str;
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  }

protected:
  void SetUp() override
  {
    // Deploy the canonical rv32 itc_erc20 contract.
    using meta = ContractTraitMeta<itc_erc20::ERC20>;
    auto rv32_contract = meta::load_rv32();
    write_contract(rv32_contract);
    write_deployment(kRv32Token, rv32_contract);
    setup_grant_storage_token(kRv32Token, 1'000'000'000);

    // Deploy the precompiled native token at the preset address.
    auto token_contract = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(token_contract);
    write_deployment(kNativeTokenContractAddress, token_contract);
    // Cover the native token's own storage writes (e.g. the allowance row).
    setup_grant_storage_token(kNativeTokenContractAddress, 10'000'000);

    // Build the wrapper contract. Each method takes the target token address as
    // its sole calldata argument and drives the shared ERC20 trait.
    NativeContractBuilder builder;
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodTransfer,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"transfer">(ctx, target, kBob,
                                                   make_amount(
                                                       kTransferAmount));
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodAdjustAllowance,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"adjust_allowance">(
              ctx, target, kSpender, make_difference(kAllowanceAmount), false);
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodTransferFrom,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"transfer_from">(
              ctx, target, kAlice, kBob, make_amount(kTransferFromAmount));
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodInitialize,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"initialize">(ctx, target, kAlice);
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodMint,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"mint">(ctx, target, kAlice,
                                               make_amount(kMintAmount));
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodInitializeGenesisRoot,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"initialize">(
              ctx, target, kGenesisRootContractAddress);
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodBurn,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"burn">(ctx, target, kAlice,
                                               make_amount(kBurnAmount));
        });
    // The netting sequence from hogd#1918: decrement a fresh (Alice, Spender)
    // allowance and then, as a sibling call in the same transaction, increment
    // it straight back. The decrement must be rejected outright -- if it were
    // instead staged and later netted away, its AllowanceChange event would
    // still be published by the committing transaction.
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodNettedNegativeAllowanceChange,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          auto rejected = co_await call_expect_err<u8"adjust_allowance">(
              ctx, target, itc_erc20::ERC20Error::TooLarge, kSpender,
              make_difference(-kAllowanceAmount), false);
          if (!rejected.has_value()) {
            co_return rejected.error();
          }
          co_return co_await call_ok<u8"adjust_allowance">(
              ctx, target, kSpender, make_difference(kAllowanceAmount), false);
        });
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kMethodClearAllowance,
        [](NativeSyscallContext &ctx,
           ContractAddress target) -> x23::Async<InvokeStatus<void>> {
          co_return co_await call_ok<u8"clear_allowance">(ctx, target,
                                                          kSpender);
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

    // Multiplex the native engine with the rv32 engine so the wrapper (native)
    // can cross-call the rv32 token.
    auto rv32_context = std::make_unique<Rv32Context>();
    multiplexed_context_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kNative, {native_context.release(), true}},
            {ContractType::kRv32, {rv32_context.release(), true}}});

    // Deploy the wrapper at the Alice and Spender addresses so that the
    // msg_sender seen by the token equals the acting party, and at the genesis
    // root address so mint/burn pass the native token's owner gate (the test
    // genesis is empty, so nothing else occupies that address).
    auto wrapper_xdr = make_native_contract(kWrapperNativeId);
    write_contract(wrapper_xdr);
    write_deployment(kAlice, wrapper_xdr);
    write_deployment(kSpender, wrapper_xdr);
    write_deployment(kGenesisRootContractAddress, wrapper_xdr);

    // Fund Alice (native balance for the native-token transfers + storage +
    // replay cache for the txs she roots), Spender (replay cache), and the
    // genesis root (replay cache for the mint/burn txs it roots).
    setup_grant_storage_token(kAlice, 50'000'000);
    setup_grant_storage_token(kSpender, 10'000'000);
    setup_grant_storage_token(kGenesisRootContractAddress, 10'000'000);
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
    ASSERT_TRUE(h.root_invoke()().is_root_success());
    ASSERT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK);
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
    EXPECT_TRUE(h.root_invoke()().is_root_success())
        << "method=" << method << " target0=" << static_cast<int>(target[0]);
    EXPECT_EQ(h.try_commit_transaction()(), ExecResult::ExecResultOK)
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

  // Ignore the emitting address, but check the topic count independently so
  // identical mistakes in both backends cannot pass the byte comparison.
  static void
  expect_events_match(std::optional<TxEmittedEvent> const &rv32_ev,
                      std::optional<TxEmittedEvent> const &native_ev,
                      uint32_t expected_topics, char const *label)
  {
    ASSERT_TRUE(rv32_ev.has_value()) << label;
    ASSERT_TRUE(native_ev.has_value()) << label;
    EXPECT_EQ(rv32_ev->num_topics, expected_topics) << label;
    EXPECT_EQ(rv32_ev->num_topics, native_ev->num_topics) << label;
    ASSERT_EQ(rv32_ev->serialized_event.size(),
              native_ev->serialized_event.size())
        << label;
    EXPECT_TRUE(std::equal(rv32_ev->serialized_event.begin(),
                           rv32_ev->serialized_event.end(),
                           native_ev->serialized_event.begin()))
        << label << ": serialized event bytes differ (topics + data)";
  }

  std::unique_ptr<MultiplexedContext> multiplexed_context_;
};

TEST_F(NativeRv32TokenEventParityTest,
       TransferAllowanceEventsMatchRv32AndNative)
{
  // --- rv32 itc_erc20: initialize + mint to Alice, then the three ops. ---
  run_setup(kMethodInitialize, kAlice, kRv32Token);
  run_setup(kMethodMint, kAlice, kRv32Token);
  auto rv32_transfer = run_op(kMethodTransfer, kAlice, kRv32Token);
  auto rv32_adjustment = run_op(kMethodAdjustAllowance, kAlice, kRv32Token);
  auto rv32_transfer_from = run_op(kMethodTransferFrom, kSpender, kRv32Token);

  // --- native token: balance was granted in SetUp, so just the three ops. ---
  ContractAddress const native_token = kNativeTokenContractAddress;
  auto native_transfer = run_op(kMethodTransfer, kAlice, native_token);
  auto native_adjustment = run_op(kMethodAdjustAllowance, kAlice, native_token);
  auto native_transfer_from =
      run_op(kMethodTransferFrom, kSpender, native_token);

  // Sanity: the events really were emitted by the two different contracts.
  ASSERT_TRUE(rv32_transfer.has_value());
  ASSERT_TRUE(native_transfer.has_value());
  EXPECT_EQ(rv32_transfer->contract_address, kRv32Token);
  EXPECT_EQ(native_transfer->contract_address, native_token);

  expect_events_match(rv32_transfer, native_transfer, 3U, "transfer");
  expect_events_match(rv32_adjustment, native_adjustment, 3U,
                      "adjust_allowance");
  expect_events_match(rv32_transfer_from, native_transfer_from, 3U,
                      "transfer_from");
}

// Mint and burn events: a supply change is its own event on
// groundhog-erc20-if, not a Transfer from or to the zero address, so a mint
// emits Mint(receiver) and a burn emits Burn(owner), each with two topics,
// byte-identically on the rv32 itc_erc20 and the native precompile (modulo
// the emitting contract address). Both run from the genesis root address: the
// native token's mint/burn are gated on it, and the rv32 token is initialized
// with it as owner so the same caller passes both gates.
TEST_F(NativeRv32TokenEventParityTest, MintBurnEventsMatchRv32AndNative)
{
  // --- rv32 itc_erc20: initialize with the genesis root as owner. ---
  run_setup(kMethodInitializeGenesisRoot, kAlice, kRv32Token);
  auto rv32_mint = run_op(kMethodMint, kGenesisRootContractAddress, kRv32Token);
  auto rv32_burn = run_op(kMethodBurn, kGenesisRootContractAddress, kRv32Token);

  // --- native token: the mint also creates the supply key the burn needs. ---
  ContractAddress const native_token = kNativeTokenContractAddress;
  auto native_mint =
      run_op(kMethodMint, kGenesisRootContractAddress, native_token);
  auto native_burn =
      run_op(kMethodBurn, kGenesisRootContractAddress, native_token);

  // Sanity: the events really were emitted by the two different contracts.
  ASSERT_TRUE(rv32_mint.has_value());
  ASSERT_TRUE(native_mint.has_value());
  EXPECT_EQ(rv32_mint->contract_address, kRv32Token);
  EXPECT_EQ(native_mint->contract_address, native_token);

  expect_events_match(rv32_mint, native_mint, 2U, "mint");
  expect_events_match(rv32_burn, native_burn, 2U, "burn");

  // Non-vacuity: mint and burn events differ (distinct signature topics and
  // distinct amounts), so the two matches above pin two distinct payloads.
  ASSERT_TRUE(rv32_burn.has_value());
  ASSERT_TRUE(native_burn.has_value());
  EXPECT_NE(rv32_mint->serialized_event, rv32_burn->serialized_event);

  // ...and neither is a Transfer wearing a different address: a mint carries
  // the Mint signature and a burn the Burn signature, which is the whole
  // point of the interface change.
  auto signature_topic = [](TxEmittedEvent const &ev) {
    Topic topic{};
    EXPECT_GE(ev.serialized_event.size(), sizeof(Topic));
    if (ev.serialized_event.size() >= sizeof(Topic)) {
      std::memcpy(topic.data(), ev.serialized_event.data(), sizeof(Topic));
    }
    return topic;
  };
  EXPECT_EQ(signature_topic(*native_mint), native_token_mint_event_signature());
  EXPECT_EQ(signature_topic(*native_burn), native_token_burn_event_signature());
  EXPECT_NE(signature_topic(*native_mint),
            native_token_transfer_event_signature());
}

// hogd#1918: a rejected decrement must not publish an event even if a later
// increment would make the transaction's final allowance nonnegative.
TEST_F(NativeRv32TokenEventParityTest,
       NettedNegativeAllowanceChangeEmitsOnlyTheValidEvent)
{
  // --- rv32 itc_erc20: initialize (no balance needed for allowances). ---
  run_setup(kMethodInitialize, kAlice, kRv32Token);
  auto rv32_adjustment =
      run_op(kMethodNettedNegativeAllowanceChange, kAlice, kRv32Token);

  // --- native token: nothing to set up beyond SetUp(). ---
  ContractAddress const native_token = kNativeTokenContractAddress;
  auto native_adjustment =
      run_op(kMethodNettedNegativeAllowanceChange, kAlice, native_token);

  // Sanity: the events really were emitted by the two different contracts.
  ASSERT_TRUE(rv32_adjustment.has_value());
  ASSERT_TRUE(native_adjustment.has_value());
  EXPECT_EQ(rv32_adjustment->contract_address, kRv32Token);
  EXPECT_EQ(native_adjustment->contract_address, native_token);

  // The single surviving event is the increment's, identical on both backends.
  expect_events_match(rv32_adjustment, native_adjustment, 3U,
                      "netted negative allowance adjustment");

  // ...and it really is the increment's event, not the rejected decrement's:
  // AllowanceChangeEvent carries the diff, so a plain +kAllowanceAmount
  // adjustment run afterwards emits a byte-identical event on each backend.
  auto rv32_plain = run_op(kMethodAdjustAllowance, kAlice, kRv32Token);
  auto native_plain = run_op(kMethodAdjustAllowance, kAlice, native_token);
  expect_events_match(rv32_adjustment, rv32_plain, 3U, "rv32 netted vs plain");
  expect_events_match(native_adjustment, native_plain, 3U,
                      "native netted vs plain");
}

TEST_F(NativeRv32TokenEventParityTest, ClearAllowanceEventsMatchRv32AndNative)
{
  run_setup(kMethodInitialize, kAlice, kRv32Token);
  run_setup(kMethodMint, kAlice, kRv32Token);
  for (auto token : {kRv32Token, kNativeTokenContractAddress}) {
    run_op(kMethodAdjustAllowance, kAlice, token);
    run_op(kMethodTransferFrom, kSpender, token);
  }

  auto rv32_clear = run_op(kMethodClearAllowance, kAlice, kRv32Token);
  auto native_clear =
      run_op(kMethodClearAllowance, kAlice, kNativeTokenContractAddress);
  expect_events_match(rv32_clear, native_clear, 3U,
                      "clear remaining allowance");
  ASSERT_TRUE(native_clear);
  auto expected = make_event_from_cxs_struct(
      native_token_allowance_change_event_signature(),
      NativeTokenAllowanceChangeEvent{
          .owner_address = kAlice,
          .spender_address = kSpender,
          .difference = to_xdr(-(kAllowanceAmount -
                                 static_cast<int128_t>(kTransferFromAmount)))});
  ASSERT_TRUE(expected);
  EXPECT_TRUE(std::ranges::equal(native_clear->serialized_event, *expected));

  auto rv32_empty = run_op(kMethodClearAllowance, kAlice, kRv32Token);
  auto native_empty =
      run_op(kMethodClearAllowance, kAlice, kNativeTokenContractAddress);
  expect_events_match(rv32_empty, native_empty, 3U, "clear missing allowance");
  ASSERT_TRUE(native_empty);
  auto expected_empty = make_event_from_cxs_struct(
      native_token_allowance_change_event_signature(),
      NativeTokenAllowanceChangeEvent{.owner_address = kAlice,
                                      .spender_address = kSpender,
                                      .difference = to_xdr(int128_t{0})});
  ASSERT_TRUE(expected_empty);
  EXPECT_TRUE(
      std::ranges::equal(native_empty->serialized_event, *expected_empty));
}

// NOLINTEND(bugprone-unchecked-optional-access)
// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
