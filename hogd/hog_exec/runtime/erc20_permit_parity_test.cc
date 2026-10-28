#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "built_contracts/itc_erc20_permit.h"
#include "built_contracts/itc_erc20_permit.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"

// Conformance tests for the native token's ERC20Permit surface: each scenario
// runs against the native precompile and the RV32 reference itc_erc20_permit,
// and the two must agree byte-for-byte.

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {

// All storage costs are zeroed, as in erc20_parity_test.cc: the native token
// and the RV32 reference charge storage differently, and parity here is about
// permit semantics, not storage pricing.
StorageCostModelXdr
zero_cost_model_xdr()
{
  return StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 0,
      .bytestring_cost_per_byte = 0,
      .base_contract_deployment_cost = 0,
      .contract_deployment_cost_per_byte = 0,
      .auth_per_key_cost = 0,
      .rcq_cost_per_entry = 0,
      .replay_cache_cost_per_entry = 0,
      .balance_cost = 0,
  };
}

using permit_meta = ContractTraitMeta<itc_erc20_permit::ERC20Permit>;
using erc20_meta = ContractTraitMeta<itc_erc20_permit::ERC20>;
using erc165_meta = ContractTraitMeta<itc_erc20_permit::ERC165>;
using interface_ids_t =
    std::tuple_element_t<0, erc165_meta::args_t<u8"supports_interfaces">>;
using interface_support_t = erc165_meta::return_t<u8"supports_interfaces">;

// A captured call outcome: a tag byte, the child frame status, and the
// literal bytes the callee returned or aborted with (see append_invoke_record).
using Record = std::vector<uint8_t>;

std::tuple<int64_t, uint64_t>
make_amount(int128_t amount)
{
  return std::make_tuple(static_cast<int64_t>(amount >> 64),
                         static_cast<uint64_t>(amount));
}

// One auth token the owner mints in a step.
struct GrantSpec {
  ContractAddress spender;
  int128_t amount;
  // The consumer the token names; the token under test when unset.
  std::optional<ContractAddress> consumer = std::nullopt;
  // Replaces the CXS-encoded PermitGrant payload when set.
  std::optional<std::vector<uint8_t>> payload = std::nullopt;
  // Child tokens attached to the grant's node.
  uint32_t child_count = 0;
  // Mint a DeleteAllowanceGrant { spender } rather than a PermitGrant.
  bool deletion = false;
};

// One call to the token in a scenario.
struct Step {
  // Who calls the token: the owner calls it directly; any other actor is a
  // relay the owner invokes, forwarding the step's handles to it.
  ContractAddress caller;
  // Grants the owner mints for this step; their handles are forwarded.
  std::vector<GrantSpec> grants;
  Runtime::MethodID method;
  Calldata calldata;
  // Forward the handles minted in this earlier step instead (and mint none).
  std::optional<size_t> reuse_grants_of = std::nullopt;
};

template<StringLiteral Name, typename... Args>
Step
permit_step(ContractAddress caller, std::vector<GrantSpec> const &grants,
            Args... args)
{
  return Step{.caller = caller,
              .grants = grants,
              .method = permit_meta::method_id<Name>(),
              .calldata = permit_meta::make_calldata<Name>(args...)};
}

template<StringLiteral Name, typename... Args>
Step
erc20_step(ContractAddress caller, Args... args)
{
  return Step{.caller = caller,
              .grants = {},
              .method = erc20_meta::method_id<Name>(),
              .calldata = erc20_meta::make_calldata<Name>(args...)};
}

template<StringLiteral Name, typename... Args>
Step
erc165_step(ContractAddress caller, Args const &...args)
{
  return Step{.caller = caller,
              .grants = {},
              .method = erc165_meta::method_id<Name>(),
              .calldata = erc165_meta::make_calldata<Name>(args...)};
}

template<is_contract_trait Trait>
InterfaceId
interface_id_of()
{
  return sdk_interface_id_derivation(Trait::kTraitName, Trait::kVersion);
}

// The (selector, CXS body) bytes a frame aborts with when it throws `exn`.
template<typename Exn>
Record
abort_bytes(uint64_t selector, Exn const &exn)
{
  auto buf = x23::serialize_cxs(selector, exn);
  EXPECT_TRUE(buf.has_value());
  return buf.has_value() ? Record(buf->begin(), buf->end()) : Record{};
}

Record
erc20_abort(itc_erc20_permit::ERC20Error error)
{
  return abort_bytes(0xf63c8b1f5e7e2fe0ULL, error);
}

Record
permit_abort(itc_erc20_permit::ERC20PermitError error)
{
  return abort_bytes(0x1aaf2b9becb7ddc4ULL, error);
}

Record
library_abort(LibraryExn const &exn)
{
  return abort_bytes(kLibraryExceptionSelector, exn);
}

Record
auth_token_abort(AuthTokenErrorKind kind)
{
  LibraryExn exn(LibraryExnKind::kLibraryExnAuthToken);
  exn.auth_token() = AuthTokenError(kind);
  return library_abort(exn);
}

Record
decoding_args_abort()
{
  LibraryExn exn(LibraryExnKind::kLibraryExnHandling);
  exn.handling() = HandlingError(HandlingErrorKind::kHandlingDecodingArgs);
  return library_abort(exn);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class Erc20PermitParityTest : public ::testing::Test,
                              public NativeContractTestFramework {
public:
  Erc20PermitParityTest() : Erc20PermitParityTest(zero_cost_model_xdr()) {}

protected:
  // Runs under `cost_model`, or the default one when unset.
  explicit Erc20PermitParityTest(
      std::optional<StorageCostModelXdr> const &cost_model)
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          /*empty_genesis=*/true, cost_model)
  {}

  // Actors, identical across the two legs so the only legitimate difference
  // between them is the token contract they call.
  static constexpr ContractAddress kOwner = make_contract_address(201);
  static constexpr ContractAddress kSpender = make_contract_address(202);
  static constexpr ContractAddress kThirdParty = make_contract_address(203);
  static constexpr ContractAddress kReceiver = make_contract_address(204);
  static constexpr ContractAddress kOtherContract = make_contract_address(205);
  static constexpr ContractAddress kRv32Token = make_contract_address(206);
  static constexpr ContractAddress kNativeToken = kNativeTokenContractAddress;

  static constexpr int128_t kOwnerFunds = 1'000'000;
  static constexpr int128_t kMaxAmount = std::numeric_limits<int128_t>::max();

  static constexpr int32_t kDriverId = 60;

  // Driver method ids: the setup calls (rooted at the genesis root), one per
  // scenario (rooted at the owner), and one relay per scenario step.
  static constexpr Runtime::MethodID kSetupInitializeMethod = 1;
  static constexpr Runtime::MethodID kSetupMintMethod = 2;
  static constexpr Runtime::MethodID kScenarioBase = 10;
  static constexpr Runtime::MethodID kRelayBase = 100;
  static constexpr Runtime::MethodID kRelaysPerScenario = 100;

  static constexpr Runtime::MethodID scenario_method(size_t scenario)
  {
    return kScenarioBase + scenario;
  }
  static constexpr Runtime::MethodID relay_method(size_t scenario, size_t step)
  {
    return kRelayBase + (scenario * kRelaysPerScenario) + step;
  }

  // Far enough out that no test reaches it.
  static constexpr BlockHeight kGrantExpiry = 1'000'000;

  // What one leg of a scenario produced.
  struct LegOutcome {
    ExecResult commit;
    std::vector<Record> records;
    std::vector<TxEmittedEvent> events;
  };

  void SetUp() override
  {
    auto rv32_token = erc20_meta::load_rv32();
    ASSERT_TRUE(rv32_token);
    write_contract(rv32_token);
    write_deployment(kRv32Token, rv32_token);

    auto native_token = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractNativeToken));
    write_contract(native_token);
    write_deployment(kNativeToken, native_token);

    auto driver = make_native_contract(kDriverId);
    write_contract(driver);
    for (auto const &addr :
         {kOwner, kSpender, kThirdParty, kGenesisRootContractAddress}) {
      write_deployment(addr, driver);
    }
  }

  // Invokes `method` on `target` forwarding `handles`, and appends a tagged
  // outcome to `record`.
  static x23::Async<void>
  append_invoke_record(NativeSyscallContext &ctx, ContractAddress const &target,
                       Runtime::MethodID method, Calldata const &calldata,
                       std::vector<uint32_t> handles, Record &record)
  {
    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{
        .entrypoint_id = method,
        .gas_limit = static_cast<GasAmount>(UINT64_MAX),
        .flags = std::to_underlying(InvokeFlag::None),
    };
    auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
        target, std::as_bytes(std::span(calldata.data(), calldata.size())),
        std::span<uint32_t>(handles), inputs, outputs);
    if (!r.has_value()) {
      // The host failed the syscall.
      record.push_back(0xEE);
      record.push_back(static_cast<uint8_t>(r.error().code()));
      co_return;
    }
    if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      // The invoke was rejected before a child frame ran.
      record.push_back(0x01);
      record.push_back(static_cast<uint8_t>(r.value()));
      co_return;
    }
    // A child frame ran: its status and literal return/abort bytes.
    record.push_back(0x00);
    record.push_back(static_cast<uint8_t>(outputs.frame_status));
    auto rb = ctx.handler().get_return_buffer();
    if (rb.has_value() && rb.value().has_value()) {
      for (auto byte_val : *rb.value().value()) {
        record.push_back(static_cast<uint8_t>(byte_val));
      }
    }
    else {
      record.push_back(0xFF);
    }
  }

  // Mints `spec` as an auth token from the running frame, returning its handle.
  static x23::Async<std::optional<uint32_t>>
  mint_grant(NativeSyscallContext &ctx, ContractAddress const &token,
             GrantSpec const &spec)
  {
    AuthTokenPayload payload;
    if (spec.payload) {
      payload.append_range(*spec.payload);
    }
    else {
      auto encoded =
          spec.deletion
              ? x23::serialize_cxs(itc_erc20_permit::DeleteAllowanceGrant{
                    .spender = spec.spender})
              : x23::serialize_cxs(itc_erc20_permit::PermitGrant{
                    .spender = spec.spender, .amount = to_xdr(spec.amount)});
      if (!encoded) {
        ADD_FAILURE() << "could not encode a grant";
        co_return std::nullopt;
      }
      payload.append_range(*encoded);
    }
    AuthTokenNode node{.consumer_address = spec.consumer.value_or(token),
                       .payload = payload,
                       .child_nodes = {}};
    for (uint32_t i = 0; i < spec.child_count; ++i) {
      node.child_nodes.push_back(
          AuthTokenNode{.consumer_address = token, .payload = payload});
    }
    auto serialized = x23::serialize(node);
    uint32_t handle = 0;
    auto r = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
        kGrantExpiry,
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(
                                       serialized.data()),
                                   serialized.size()),
        handle);
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "kMintAuthToken failed";
      co_return std::nullopt;
    }
    co_return handle;
  }

  static StorageKeyContractData step_key(size_t scenario, size_t i)
  {
    return make_storage_key("permit_s" + std::to_string(scenario) + "_step_" +
                            std::to_string(i));
  }

  // The driver contract for one leg: every method's token calls target
  // `token`.
  static std::unique_ptr<NativeContract>
  make_driver(ContractAddress token, std::vector<std::vector<Step>> scenarios)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;

    fns.emplace(kSetupInitializeMethod,
                [token](NativeSyscallContext &ctx)
                    -> x23::Async<InvokeStatus<void>> {
                  Record record;
                  co_await append_invoke_record(
                      ctx, token, erc20_meta::method_id<u8"initialize">(),
                      erc20_meta::make_calldata<u8"initialize">(
                          kGenesisRootContractAddress),
                      {}, record);
                  EXPECT_EQ(record, ok()) << "initialize failed";
                  co_return InvokeStatus<void>{};
                });
    fns.emplace(
        kSetupMintMethod,
        [token](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          Record record;
          co_await append_invoke_record(ctx, token,
                                        erc20_meta::method_id<u8"mint">(),
                                        erc20_meta::make_calldata<u8"mint">(
                                            kOwner, make_amount(kOwnerFunds)),
                                        {}, record);
          EXPECT_EQ(record, ok()) << "mint failed";
          co_return InvokeStatus<void>{};
        });

    for (size_t sc = 0; sc < scenarios.size(); ++sc) {
      auto const &steps = scenarios[sc];
      for (size_t i = 0; i < steps.size(); ++i) {
        // The relay: forward whatever handles the owner passed down.
        fns.emplace(
            relay_method(sc, i),
            [token, step = steps[i]](
                NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
              std::vector<uint32_t> parent(
                  ctx.handler().get_parent_supplied_auth_token_count());
              std::ranges::iota(parent, uint32_t{0});
              Record record;
              co_await append_invoke_record(ctx, token, step.method,
                                            step.calldata, parent, record);
              ctx.handler().set_return_buffer(ReturnBuffer(record.begin(),
                                                           record.end()),
                                              false, {});
              co_return InvokeStatus<void>{};
            });
      }

      fns.emplace(
          scenario_method(sc),
          [token, sc,
           steps](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
            std::vector<std::vector<uint32_t>> minted(steps.size());
            for (size_t i = 0; i < steps.size(); ++i) {
              auto const &step = steps[i];
              if (step.reuse_grants_of) {
                minted[i] = minted[*step.reuse_grants_of];
              }
              for (auto const &grant : step.grants) {
                auto handle = co_await mint_grant(ctx, token, grant);
                if (!handle) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                minted[i].push_back(*handle);
              }

              Record record;
              if (step.caller == kOwner) {
                co_await append_invoke_record(ctx, token, step.method,
                                              step.calldata, minted[i], record);
              }
              else {
                co_await append_invoke_record(ctx, step.caller,
                                              relay_method(sc, i), {},
                                              minted[i], record);
              }

              auto set = co_await ctx.syscall<SyscallNo::kBytestringSet>(
                  step_key(sc, i),
                  std::as_bytes(std::span(record.data(), record.size())));
              if (!set.has_value() ||
                  *set != SyscallErrorCode::kSyscallErrorCodeNone) {
                ADD_FAILURE() << "could not persist step " << i;
                co_return InvokeError{InvokeErrorCode::Deterministic};
              }
            }
            co_return InvokeStatus<void>{};
          });
    }

    return std::make_unique<NativeContract>(std::move(fns));
  }

  // Commits a transaction rooted at `root` calling the driver's `method`.
  // Returns the commit result, and the transaction's id for its event log.
  std::pair<ExecResult, EphemeralTxIdentifier>
  run_driver_tx(RuntimeContext &ctx, ContractAddress root,
                Runtime::MethodID method, bool advance = true)
  {
    EphemeralTxIdentifier tx_id;
    SignedTransaction tx{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = root,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = kDefaultMaxTxGas,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(tx.tx, ctx, &tx_id);
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    ExecResult commit = handler.try_commit_transaction()();
    if (commit == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                      ProposedTransaction{.signed_tx = tx})());
    }
    if (advance) {
      advance_block();
    }
    return {commit, tx_id};
  }

  Record read_record(size_t scenario, size_t i)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = step_key(scenario, i);
    auto item = lookup_storage_item_from_db(
        AddressAndKey{.address = kOwner, .key = std::move(key)});
    EXPECT_TRUE(item.has_value()) << "no record for step " << i;
    if (!item.has_value()) {
      return {};
    }
    auto const &bs = item->bytestring();
    return Record(bs.begin(), bs.end());
  }

  // Runs `scenarios` against `token` (after initializing the RV32 reference
  // and funding the owner), one block per entry of `blocks`; returns one
  // outcome per scenario. `after_block(n)` runs once setup is done (n = 0)
  // and after each block (n = blocks run so far).
  std::vector<LegOutcome>
  run_leg(ContractAddress token,
          std::vector<std::vector<Step>> const &scenarios,
          std::vector<std::vector<size_t>> const &blocks,
          std::function<void(size_t)> const &after_block = {})
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(kDriverId, make_driver(token, scenarios));
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
        make_native_token_contract());
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    if (token == kRv32Token) {
      EXPECT_EQ(run_driver_tx(ctx, kGenesisRootContractAddress,
                              kSetupInitializeMethod)
                    .first,
                ExecResult::ExecResultOK);
    }
    EXPECT_EQ(
        run_driver_tx(ctx, kGenesisRootContractAddress, kSetupMintMethod).first,
        ExecResult::ExecResultOK);

    if (after_block) {
      after_block(0);
    }
    std::vector<LegOutcome> out(scenarios.size());
    for (size_t b = 0; b < blocks.size(); ++b) {
      auto const &block = blocks[b];
      std::vector<std::pair<size_t, EphemeralTxIdentifier>> committed;
      for (size_t sc : block) {
        auto [commit, tx_id] = run_driver_tx(ctx, kOwner, scenario_method(sc),
                                             /*advance=*/false);
        out[sc].commit = commit;
        if (commit == ExecResult::ExecResultOK) {
          committed.emplace_back(sc, tx_id);
        }
      }
      advance_block();
      for (auto const &[sc, tx_id] : committed) {
        for (size_t i = 0; i < scenarios[sc].size(); ++i) {
          out[sc].records.push_back(read_record(sc, i));
        }
        auto log = get_tx_event_log(tx_id)();
        EXPECT_TRUE(log.has_value());
        if (log.has_value()) {
          out[sc].events.assign(log->events.begin(), log->events.end());
        }
      }
      if (after_block) {
        after_block(b + 1);
      }
    }
    return out;
  }

  // Requires the two backends' outcomes for one scenario to match.
  static void expect_outcomes_match(LegOutcome const &native,
                                    LegOutcome const &rv32)
  {
    EXPECT_EQ(native.commit, rv32.commit);
    EXPECT_EQ(native.records.size(), rv32.records.size());
    for (size_t i = 0; i < native.records.size() && i < rv32.records.size();
         ++i) {
      EXPECT_EQ(native.records[i], rv32.records[i])
          << "step " << i << " differs between the native token and the "
          << "RV32 reference";
    }

    EXPECT_EQ(native.events.size(), rv32.events.size());
    for (size_t i = 0; i < native.events.size() && i < rv32.events.size();
         ++i) {
      EXPECT_EQ(native.events[i].contract_address, kNativeToken);
      EXPECT_EQ(rv32.events[i].contract_address, kRv32Token);
      EXPECT_EQ(native.events[i].num_topics, rv32.events[i].num_topics)
          << "event " << i;
      EXPECT_EQ(native.events[i].serialized_event,
                rv32.events[i].serialized_event)
          << "event " << i << " differs (topics + data)";
    }
  }

  // Runs `scenarios` on both backends under the `blocks` schedule, requiring
  // parity scenario by scenario; returns the native outcomes.
  std::vector<LegOutcome>
  expect_schedule_parity(std::vector<std::vector<Step>> const &scenarios,
                         std::vector<std::vector<size_t>> const &blocks)
  {
    auto native = run_leg(kNativeToken, scenarios, blocks);
    auto rv32 = run_leg(kRv32Token, scenarios, blocks);
    for (size_t sc = 0; sc < scenarios.size(); ++sc) {
      SCOPED_TRACE("scenario " + std::to_string(sc));
      expect_outcomes_match(native[sc], rv32[sc]);
    }
    return native;
  }

  // Runs `steps` on both backends, requires byte parity, and returns the
  // native leg's per-step records with any relay framing removed, so a test
  // can pin what each step did.
  std::vector<Record> expect_parity(std::vector<Step> const &steps)
  {
    auto native = std::move(expect_schedule_parity({steps}, {{0}})[0]);
    EXPECT_EQ(native.commit, ExecResult::ExecResultOK);

    std::vector<Record> results;
    for (size_t i = 0; i < native.records.size() && i < steps.size(); ++i) {
      results.push_back(unwrap_relay(steps[i], native.records[i]));
    }
    native_events_ = std::move(native.events);
    return results;
  }

  // A relayed step's record is the relay's frame around the token call's
  // record; strip it after checking the relay itself returned.
  static Record unwrap_relay(Step const &step, Record const &record)
  {
    if (step.caller == kOwner) {
      return record;
    }
    if (record.size() < 2 || record[0] != 0x00 ||
        static_cast<InvocationFrameStatusCode>(record[1]) !=
            InvocationFrameStatusCode::Return) {
      ADD_FAILURE() << "relay did not return";
      return {};
    }
    return Record(record.begin() + 2, record.end());
  }

  static Record returned(Record const &payload)
  {
    Record r{0x00, static_cast<uint8_t>(InvocationFrameStatusCode::Return)};
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
  }

  static Record aborted(Record const &payload)
  {
    Record r{0x00, static_cast<uint8_t>(InvocationFrameStatusCode::Abort)};
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
  }

  // The record of a call returning Ok(()).
  static Record ok() { return returned({}); }

  // The record of an ERC20 read returning Ok(amount).
  static Record ok_amount(int128_t amount)
  {
    auto buf =
        Serializer<SerializationFormat::kCxs>::serialize(make_amount(amount));
    EXPECT_TRUE(buf.has_value());
    return returned(Record(buf->begin(), buf->end()));
  }

  // The record of a call returning `value`, outside any Result.
  template<typename T> static Record ok_value(T const &value)
  {
    auto buf = Serializer<SerializationFormat::kCxs>::serialize(value);
    EXPECT_TRUE(buf.has_value());
    return returned(Record(buf->begin(), buf->end()));
  }

  std::vector<TxEmittedEvent> native_events_;
};

// A signed increase writes an allowance the spender then draws on through the
// ordinary transfer_from, exactly as an adjust_allowance by the owner would.
TEST_F(Erc20PermitParityTest, IncreaseAllowanceSignedIsSpentByTransferFrom)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 500}}, SignedToken{}),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
      erc20_step<u8"transfer_from">(kSpender, kOwner, kReceiver,
                                    make_amount(300)),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
      erc20_step<u8"transfer_from">(kSpender, kOwner, kReceiver,
                                    make_amount(201)),
      erc20_step<u8"get_balance_of">(kSpender, kReceiver),
  });
  ASSERT_EQ(results.size(), 6U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok_amount(500));
  EXPECT_EQ(results[2], ok());
  EXPECT_EQ(results[3], ok_amount(200));
  EXPECT_EQ(results[4],
            aborted(erc20_abort(itc_erc20_permit::ERC20Error::TooLarge)));
  EXPECT_EQ(results[5], ok_amount(300));
  // AllowanceChange for the increase, Transfer for the successful spend.
  EXPECT_EQ(native_events_.size(), 2U);
}

// Anyone may submit a grant; the owner is its minter, never the
// caller. Here the third party submits, and the owner submits its own.
TEST_F(Erc20PermitParityTest, OwnerIsMinterWhoeverSubmits)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kThirdParty, {{.spender = kSpender, .amount = 70}}, SignedToken{}),
      permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 30}}, SignedToken{}),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
      erc20_step<u8"allowance">(kSpender, kThirdParty, kSpender),
  });
  ASSERT_EQ(results.size(), 4U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok());
  // A grant increases the allowance by its amount rather than setting it.
  EXPECT_EQ(results[2], ok_amount(100));
  EXPECT_EQ(results[3], ok_amount(0));
}

// A signed transfer spends the requested amount without creating an allowance.
TEST_F(Erc20PermitParityTest, TransferFromSignedMovesWithoutAllowance)
{
  auto results = expect_parity({
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 400}}, SignedToken{},
          kReceiver, make_amount(400)),
      erc20_step<u8"get_balance_of">(kSpender, kReceiver),
      erc20_step<u8"get_balance_of">(kSpender, kOwner),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 4U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok_amount(400));
  EXPECT_EQ(results[2], ok_amount(kOwnerFunds - 400));
  EXPECT_EQ(results[3], ok_amount(0));
  EXPECT_EQ(native_events_.size(), 1U);
}

// Only the grant's spender may spend it: a third party, or the owner itself,
// presenting the grant is rejected with NotSpender after the grant is consumed.
TEST_F(Erc20PermitParityTest, TransferFromSignedRejectsAnyoneButSpender)
{
  auto results = expect_parity({
      permit_step<u8"transfer_from_signed">(
          kThirdParty, {{.spender = kSpender, .amount = 10}}, SignedToken{},
          kThirdParty, make_amount(10)),
      permit_step<u8"transfer_from_signed">(
          kOwner, {{.spender = kSpender, .amount = 10}}, SignedToken{},
          kReceiver, make_amount(10)),
      erc20_step<u8"get_balance_of">(kSpender, kOwner),
  });
  ASSERT_EQ(results.size(), 3U);
  auto not_spender =
      aborted(permit_abort(itc_erc20_permit::ERC20PermitError::NotSpender));
  EXPECT_EQ(results[0], not_spender);
  EXPECT_EQ(results[1], not_spender);
  EXPECT_EQ(results[2], ok_amount(kOwnerFunds));
  EXPECT_TRUE(native_events_.empty());
}

// The owner's running balance bounds a signed transfer as it bounds
// transfer_from: all of it may move, one more is TooLarge.
TEST_F(Erc20PermitParityTest, TransferFromSignedBalanceBoundary)
{
  auto results = expect_parity({
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = kOwnerFunds + 1}},
          SignedToken{}, kReceiver, make_amount(kOwnerFunds + 1)),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = kOwnerFunds}},
          SignedToken{}, kReceiver, make_amount(kOwnerFunds)),
      erc20_step<u8"get_balance_of">(kSpender, kReceiver),
  });
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0],
            aborted(erc20_abort(itc_erc20_permit::ERC20Error::TooLarge)));
  EXPECT_EQ(results[1], ok());
  EXPECT_EQ(results[2], ok_amount(kOwnerFunds));
}

// A negative amount, granted or requested, is NegativeAmount on both paths,
// checked after the grant's own checks, and leaves the standing allowance and
// both balances untouched. INT128_MIN is the extreme of the signed decoding
// and the one value that cannot be negated, so it runs beside -1 on every
// spending path, the base ERC20 ones included.
TEST_F(Erc20PermitParityTest, NegativeAmountIsRejected)
{
  constexpr int128_t kMin = std::numeric_limits<int128_t>::min();
  std::vector<Step> steps{permit_step<u8"increase_allowance_signed">(
      kSpender, {{.spender = kSpender, .amount = 100}}, SignedToken{})};
  for (int128_t amount : {int128_t{-1}, kMin}) {
    steps.push_back(permit_step<u8"increase_allowance_signed">(
        kSpender, {{.spender = kSpender, .amount = amount}}, SignedToken{}));
    steps.push_back(permit_step<u8"transfer_from_signed">(
        kSpender, {{.spender = kSpender, .amount = 10}}, SignedToken{},
        kReceiver, make_amount(amount)));
    steps.push_back(permit_step<u8"transfer_from_signed">(
        kSpender, {{.spender = kSpender, .amount = amount}}, SignedToken{},
        kReceiver, make_amount(amount)));
    steps.push_back(erc20_step<u8"transfer_from">(kSpender, kOwner, kReceiver,
                                                  make_amount(amount)));
    steps.push_back(
        erc20_step<u8"transfer">(kOwner, kReceiver, make_amount(amount)));
  }
  steps.push_back(erc20_step<u8"allowance">(kSpender, kOwner, kSpender));
  steps.push_back(erc20_step<u8"get_balance_of">(kSpender, kOwner));
  steps.push_back(erc20_step<u8"get_balance_of">(kSpender, kReceiver));
  // INT128_MAX itself is a representable allowance.
  steps.push_back(permit_step<u8"increase_allowance_signed">(
      kThirdParty, {{.spender = kThirdParty, .amount = kMaxAmount}},
      SignedToken{}));

  auto results = expect_parity(steps);
  ASSERT_EQ(results.size(), 15U);
  EXPECT_EQ(results[0], ok());
  auto negative =
      aborted(erc20_abort(itc_erc20_permit::ERC20Error::NegativeAmount));
  for (std::size_t i = 1; i <= 10; ++i) {
    EXPECT_EQ(results[i], negative) << "step " << i;
  }
  EXPECT_EQ(results[11], ok_amount(100));
  EXPECT_EQ(results[12], ok_amount(kOwnerFunds));
  EXPECT_EQ(results[13], ok_amount(0));
  EXPECT_EQ(results[14], ok());
  // Only the two increases emit; every rejected call publishes nothing.
  EXPECT_EQ(native_events_.size(), 2U);
}

// 2^69 has a high word of 32: past the 31 that CXS encodes inline for a
// signed integer, though within the 62 it would for an unsigned one, so it
// pins the i128 Amount encoding that small values and INT128_MAX both miss.
// A grant of that size roundtrips through the grant payload, the allowance
// write and the allowance read, and spends like any other.
TEST_F(Erc20PermitParityTest, AmountAboveTheSignedInlineRangeRoundtrips)
{
  constexpr int128_t kLarge = int128_t{1} << 69;
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = kLarge}}, SignedToken{}),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
      erc20_step<u8"transfer_from">(kSpender, kOwner, kReceiver,
                                    make_amount(10)),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = kLarge}}, SignedToken{},
          kReceiver, make_amount(20)),
      erc20_step<u8"get_balance_of">(kSpender, kReceiver),
  });
  ASSERT_EQ(results.size(), 6U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok_amount(kLarge));
  EXPECT_EQ(results[2], ok());
  EXPECT_EQ(results[3], ok_amount(kLarge - 10));
  EXPECT_EQ(results[4], ok());
  EXPECT_EQ(results[5], ok_amount(30));
}

// A self-grant is an ordinary allowance; a signed transfer does not consume it.
TEST_F(Erc20PermitParityTest, GrantToSelf)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kOwner, .amount = 5}}, SignedToken{}),
      erc20_step<u8"allowance">(kOwner, kOwner, kOwner),
      permit_step<u8"transfer_from_signed">(kOwner,
                                            {{.spender = kOwner, .amount = 5}},
                                            SignedToken{}, kReceiver,
                                            make_amount(5)),
      erc20_step<u8"allowance">(kOwner, kOwner, kOwner),
      permit_step<u8"delete_allowance_signed">(
          kThirdParty, {{.spender = kOwner, .amount = 0, .deletion = true}},
          SignedToken{}),
      erc20_step<u8"allowance">(kOwner, kOwner, kOwner),
  });
  ASSERT_EQ(results.size(), 6U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok_amount(5));
  EXPECT_EQ(results[2], ok());
  EXPECT_EQ(results[3], ok_amount(5));
  EXPECT_EQ(results[4], ok());
  EXPECT_EQ(results[5], ok_amount(0));
  EXPECT_EQ(native_events_.size(), 3U);
}

// A grant minted for another consumer reaches the token but cannot be consumed
// there; this also pins the unsigned encoding of syscall number 32.
TEST_F(Erc20PermitParityTest, GrantForAnotherConsumerIsNotConsumable)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kSpender,
          {{.spender = kSpender, .amount = 5, .consumer = kOtherContract}},
          SignedToken{}),
      permit_step<u8"transfer_from_signed">(
          kSpender,
          {{.spender = kSpender, .amount = 5, .consumer = kOtherContract}},
          SignedToken{}, kReceiver, make_amount(5)),
  });
  ASSERT_EQ(results.size(), 2U);
  auto refused = aborted(
      library_abort(NativeContractBuilder::syscall_exception(
                        SyscallNo::kConsumeAuthToken,
                        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput)
                        .exn));
  EXPECT_EQ(results[0], refused);
  EXPECT_EQ(results[1], refused);
}

// The handles a call carries must match its Signed arguments one for one, as
// the SDK's entrypoint glue requires; a method without Signed arguments still
// takes none.
TEST_F(Erc20PermitParityTest, HandleCountMustMatchSignedArguments)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(kSpender, {}, SignedToken{}),
      permit_step<u8"increase_allowance_signed">(
          kSpender,
          {{.spender = kSpender, .amount = 1},
           {.spender = kSpender, .amount = 2}},
          SignedToken{}),
      permit_step<u8"transfer_from_signed">(kSpender, {}, SignedToken{},
                                            kReceiver, make_amount(1)),
      Step{.caller = kOwner,
           .grants = {{.spender = kSpender, .amount = 1}},
           .method = erc20_meta::method_id<u8"transfer">(),
           .calldata = erc20_meta::make_calldata<u8"transfer">(kReceiver,
                                                               make_amount(1))},
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 5U);
  auto decoding_args = aborted(decoding_args_abort());
  EXPECT_EQ(results[0], decoding_args);
  EXPECT_EQ(results[1], decoding_args);
  EXPECT_EQ(results[2], decoding_args);
  EXPECT_EQ(results[3], decoding_args);
  EXPECT_EQ(results[4], ok_amount(0));
}

// A grant's payload must be exactly one CXS PermitGrant, and a PermitGrant has
// no Signed field to take child tokens.
TEST_F(Erc20PermitParityTest, MalformedGrantsFailAtConsume)
{
  auto valid = x23::serialize_cxs(
      itc_erc20_permit::PermitGrant{.spender = kSpender,
                                    .amount = to_xdr(int128_t{5})});
  ASSERT_TRUE(valid.has_value());
  std::vector<uint8_t> trailing(valid->begin(), valid->end());
  trailing.push_back(0);
  std::vector<uint8_t> truncated(valid->begin(), valid->end() - 1);

  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 5, .payload = trailing}},
          SignedToken{}),
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 5, .payload = truncated}},
          SignedToken{}),
      permit_step<u8"increase_allowance_signed">(kSpender,
                                                 {{.spender = kSpender,
                                                   .amount = 5,
                                                   .payload =
                                                       std::vector<uint8_t>{}}},
                                                 SignedToken{}),
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 5, .child_count = 1}},
          SignedToken{}),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 5, .child_count = 2}},
          SignedToken{}, kReceiver, make_amount(5)),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 6U);
  auto decode_failure = aborted(
      auth_token_abort(AuthTokenErrorKind::kAuthTokenConsumeDecodeFailure));
  auto children = aborted(auth_token_abort(
      AuthTokenErrorKind::kAuthTokenReceiveAuthHandlesFailure));
  EXPECT_EQ(results[0], decode_failure);
  EXPECT_EQ(results[1], decode_failure);
  EXPECT_EQ(results[2], decode_failure);
  EXPECT_EQ(results[3], children);
  EXPECT_EQ(results[4], children);
  EXPECT_EQ(results[5], ok_amount(0));
  EXPECT_TRUE(native_events_.empty());
}

// A grant is spent by the call that consumes it: presenting its handle again
// is refused before any token frame runs.
TEST_F(Erc20PermitParityTest, ConsumedGrantCannotBeReplayed)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 9}}, SignedToken{}),
      Step{.caller = kOwner,
           .grants = {},
           .method = permit_meta::method_id<u8"increase_allowance_signed">(),
           .calldata =
               permit_meta::make_calldata<u8"increase_allowance_signed">(
                   SignedToken{}),
           .reuse_grants_of = 0},
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0], ok());
  ASSERT_FALSE(results[1].empty());
  EXPECT_EQ(results[1][0], 0x01) << "the replay reached a token frame";
  EXPECT_EQ(results[2], ok_amount(9));
}

// A grant is a ceiling: the spender moves what it asks for, up to the grant,
// and the unused remainder goes nowhere.
TEST_F(Erc20PermitParityTest, TransferFromSignedMovesUpToTheGrant)
{
  auto results = expect_parity({
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 400}}, SignedToken{},
          kReceiver, make_amount(150)),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 400}}, SignedToken{},
          kReceiver, make_amount(0)),
      erc20_step<u8"get_balance_of">(kSpender, kReceiver),
      erc20_step<u8"get_balance_of">(kSpender, kOwner),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 5U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok());
  EXPECT_EQ(results[2], ok_amount(150));
  EXPECT_EQ(results[3], ok_amount(kOwnerFunds - 150));
  EXPECT_EQ(results[4], ok_amount(0));
  EXPECT_EQ(native_events_.size(), 2U);
}

// Asking for more than the grant is ExceedsGrant, checked after the spender
// (a stranger asking too much is still NotSpender) and before the amount's
// sign (a request above a negative grant is still ExceedsGrant).
TEST_F(Erc20PermitParityTest, TransferFromSignedRefusesMoreThanTheGrant)
{
  auto results = expect_parity({
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 10}}, SignedToken{},
          kReceiver, make_amount(11)),
      permit_step<u8"transfer_from_signed">(
          kThirdParty, {{.spender = kSpender, .amount = 10}}, SignedToken{},
          kReceiver, make_amount(11)),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = -5}}, SignedToken{},
          kReceiver, make_amount(-1)),
      erc20_step<u8"get_balance_of">(kSpender, kOwner),
  });
  ASSERT_EQ(results.size(), 4U);
  auto exceeds =
      aborted(permit_abort(itc_erc20_permit::ERC20PermitError::ExceedsGrant));
  EXPECT_EQ(results[0], exceeds);
  EXPECT_EQ(results[1], aborted(permit_abort(
                            itc_erc20_permit::ERC20PermitError::NotSpender)));
  EXPECT_EQ(results[2], exceeds);
  EXPECT_EQ(results[3], ok_amount(kOwnerFunds));
  EXPECT_TRUE(native_events_.empty());
}

// A signed delete removes the allowance, reporting the amount removed, and a
// write later in the same transaction applies after it.
TEST_F(Erc20PermitParityTest, DeleteAllowanceSignedRemovesTheAllowance)
{
  auto results = expect_parity({
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 500}}, SignedToken{}),
      permit_step<u8"delete_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 0, .deletion = true}},
          SignedToken{}),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
      erc20_step<u8"transfer_from">(kSpender, kOwner, kReceiver,
                                    make_amount(1)),
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 7}}, SignedToken{}),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 6U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok());
  EXPECT_EQ(results[2], ok_amount(0));
  EXPECT_EQ(results[3],
            aborted(erc20_abort(itc_erc20_permit::ERC20Error::TooLarge)));
  EXPECT_EQ(results[4], ok());
  EXPECT_EQ(results[5], ok_amount(7));
  // AllowanceChange(+500), AllowanceDelete(500), then AllowanceChange(+7).
  EXPECT_EQ(native_events_.size(), 3U);
}

// Deleting a missing allowance succeeds, including an owner/spender self-pair.
TEST_F(Erc20PermitParityTest, DeleteAllowanceSignedEdgeCases)
{
  auto results = expect_parity({
      permit_step<u8"delete_allowance_signed">(
          kThirdParty, {{.spender = kSpender, .amount = 0, .deletion = true}},
          SignedToken{}),
      permit_step<u8"delete_allowance_signed">(
          kOwner, {{.spender = kOwner, .amount = 0, .deletion = true}},
          SignedToken{}),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0], ok());
  EXPECT_EQ(results[1], ok());
  EXPECT_EQ(results[2], ok_amount(0));
}

// A grant's payload is the type its request signed for: redeemed at another
// method it fails to decode, whichever way round.
TEST_F(Erc20PermitParityTest, GrantRedeemedForTheWrongOperationFailsToDecode)
{
  auto results = expect_parity({
      permit_step<u8"delete_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 5}}, SignedToken{}),
      permit_step<u8"increase_allowance_signed">(
          kSpender, {{.spender = kSpender, .amount = 0, .deletion = true}},
          SignedToken{}),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 0, .deletion = true}},
          SignedToken{}, kReceiver, make_amount(0)),
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 4U);
  auto decode_failure = aborted(
      auth_token_abort(AuthTokenErrorKind::kAuthTokenConsumeDecodeFailure));
  EXPECT_EQ(results[0], decode_failure);
  EXPECT_EQ(results[1], decode_failure);
  EXPECT_EQ(results[2], decode_failure);
  EXPECT_EQ(results[3], ok_amount(0));
}

// ERC-165: the native token claims the interfaces the RV32 reference's impls
// put in its interface table -- ERC20, ERC20Permit and ERC165 itself -- and
// nothing else, one id at a time or as a list.
TEST_F(Erc20PermitParityTest, SupportsInterfaceMatchesTheReference)
{
  auto const erc20 = interface_id_of<itc_erc20_permit::ERC20>();
  auto const permit = interface_id_of<itc_erc20_permit::ERC20Permit>();
  auto const erc165 = interface_id_of<itc_erc20_permit::ERC165>();
  // ERC20 at the next major is a different interface.
  auto const erc20_next_major =
      sdk_interface_id_derivation(u8"ERC20", u8"0.2.0");
  InterfaceId const unknown{1, 2, 3, 4, 5, 6, 7, 8};

  interface_ids_t query;
  for (auto const &id : {erc20, unknown, erc165, permit, erc20}) {
    query.push_back(id);
  }
  interface_support_t answer;
  for (bool b : {true, false, true, true, true}) {
    answer.push_back(b);
  }

  auto results = expect_parity({
      erc165_step<u8"supports_interface">(kOwner, erc20),
      erc165_step<u8"supports_interface">(kOwner, permit),
      erc165_step<u8"supports_interface">(kOwner, erc165),
      erc165_step<u8"supports_interface">(kOwner, erc20_next_major),
      erc165_step<u8"supports_interface">(kOwner, unknown),
      erc165_step<u8"supports_interfaces">(kSpender, query),
      erc165_step<u8"supports_interfaces">(kSpender, interface_ids_t{}),
  });
  ASSERT_EQ(results.size(), 7U);
  EXPECT_EQ(results[0], ok_value(true));
  EXPECT_EQ(results[1], ok_value(true));
  EXPECT_EQ(results[2], ok_value(true));
  EXPECT_EQ(results[3], ok_value(false));
  EXPECT_EQ(results[4], ok_value(false));
  EXPECT_EQ(results[5], ok_value(answer));
  EXPECT_EQ(results[6], ok_value(interface_support_t{}));
  EXPECT_TRUE(native_events_.empty());
}

// A delete and an increase of the same allowance cannot both land in one block:
// the later transaction fails at commit, on both backends.
TEST_F(Erc20PermitParityTest, DeleteConflictsWithAnIncreaseInTheSameBlock)
{
  auto outcomes = expect_schedule_parity(
      {
          {permit_step<u8"increase_allowance_signed">(
              kSpender, {{.spender = kSpender, .amount = 500}}, SignedToken{})},
          {permit_step<u8"delete_allowance_signed">(
              kSpender, {{.spender = kSpender, .amount = 0, .deletion = true}},
              SignedToken{})},
          {permit_step<u8"increase_allowance_signed">(
              kThirdParty, {{.spender = kSpender, .amount = 70}},
              SignedToken{})},
          {erc20_step<u8"allowance">(kOwner, kOwner, kSpender)},
      },
      {{0}, {1, 2}, {3}});
  ASSERT_EQ(outcomes.size(), 4U);
  EXPECT_EQ(outcomes[0].commit, ExecResult::ExecResultOK);
  EXPECT_EQ(outcomes[1].commit, ExecResult::ExecResultOK);
  EXPECT_NE(outcomes[2].commit, ExecResult::ExecResultOK)
      << "the increase landed beside the delete";
  ASSERT_EQ(outcomes[3].records.size(), 1U);
  EXPECT_EQ(outcomes[3].records[0], ok_amount(0));
}

// The permit surface under the default storage cost model. The two backends
// charge storage differently overall, but an allowance entry is one
// nonnegative-int item on both, charged to the token contract.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class Erc20PermitStorageRefundTest : public Erc20PermitParityTest {
public:
  Erc20PermitStorageRefundTest() : Erc20PermitParityTest(std::nullopt) {}

protected:
  static constexpr int128_t kActorFunds = 1'000'000'000'000;

  static int128_t entry_cost()
  {
    return get_default_cost_model()->get_nnint_cost();
  }

  // Funds every actor well past what the scenarios cost, and `token` with
  // `token_funds`.
  void fund(ContractAddress token, int128_t token_funds)
  {
    for (auto const &addr :
         {kOwner, kSpender, kThirdParty, kGenesisRootContractAddress}) {
      setup_grant_storage_token(addr, kActorFunds);
    }
    setup_grant_storage_token(token, token_funds);
  }

  int128_t storage_balance(ContractAddress const &addr)
  {
    auto item = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(addr));
    EXPECT_TRUE(item.has_value());
    return item.has_value() ? from_xdr(item->nnint()) : 0;
  }

  // Runs `scenarios` on `token` under `blocks`, recording the token contract's
  // storage balance once setup is done and after each block.
  std::vector<LegOutcome>
  run_recording_balance(ContractAddress token,
                        std::vector<std::vector<Step>> const &scenarios,
                        std::vector<std::vector<size_t>> const &blocks,
                        std::vector<int128_t> &balances)
  {
    return run_leg(token, scenarios, blocks,
                   [&](size_t) { balances.push_back(storage_balance(token)); });
  }
};

// Deleting an allowance refunds its entry's storage cost to the token
// contract, on both backends.
TEST_F(Erc20PermitStorageRefundTest, DeleteAllowanceSignedRefundsTheEntry)
{
  std::vector<std::vector<Step>> const scenarios{
      {permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 500}}, SignedToken{})},
      {permit_step<u8"delete_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 0, .deletion = true}},
          SignedToken{})},
  };
  std::vector<std::vector<LegOutcome>> legs;
  for (auto token : {kNativeToken, kRv32Token}) {
    SCOPED_TRACE(token == kNativeToken ? "native" : "rv32");
    fund(token, kActorFunds);
    std::vector<int128_t> balances;
    legs.push_back(
        run_recording_balance(token, scenarios, {{0}, {1}}, balances));
    ASSERT_EQ(balances.size(), 3U);
    EXPECT_EQ(balances[0] - balances[1], entry_cost()) << "the entry's cost";
    EXPECT_EQ(balances[2] - balances[1], entry_cost()) << "the refund";
    for (auto const &outcome : legs.back()) {
      EXPECT_EQ(outcome.commit, ExecResult::ExecResultOK);
    }
  }
  for (size_t sc = 0; sc < scenarios.size(); ++sc) {
    expect_outcomes_match(legs[0][sc], legs[1][sc]);
  }
}

TEST_F(Erc20PermitStorageRefundTest, DeleteAllowanceRefundsTheEntryOnce)
{
  std::vector<std::vector<Step>> const scenarios{
      {permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 500}}, SignedToken{})},
      {erc20_step<u8"delete_allowance">(kOwner, kSpender),
       erc20_step<u8"allowance">(kOwner, kOwner, kSpender)},
      {erc20_step<u8"delete_allowance">(kOwner, kSpender),
       erc20_step<u8"allowance">(kOwner, kOwner, kSpender)},
  };
  std::vector<std::vector<LegOutcome>> legs;
  for (auto token : {kNativeToken, kRv32Token}) {
    SCOPED_TRACE(token == kNativeToken ? "native" : "rv32");
    fund(token, kActorFunds);
    std::vector<int128_t> balances;
    legs.push_back(
        run_recording_balance(token, scenarios, {{0}, {1}, {2}}, balances));
    auto const &out = legs.back();
    ASSERT_EQ(balances.size(), 4U);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(balances[0] - balances[1], entry_cost());
    EXPECT_EQ(balances[2], balances[0]);
    EXPECT_EQ(balances[3], balances[2])
        << "missing entry must not refund again";
    for (auto const &outcome : out) {
      EXPECT_EQ(outcome.commit, ExecResult::ExecResultOK);
    }
    ASSERT_EQ(out[0].records.size(), 1U);
    EXPECT_EQ(out[0].records[0], ok());
    for (size_t sc : {1U, 2U}) {
      ASSERT_EQ(out[sc].records.size(), 2U);
      EXPECT_EQ(out[sc].records[0], ok());
      EXPECT_EQ(out[sc].records[1], ok_amount(0));
    }
  }
  for (size_t sc = 0; sc < scenarios.size(); ++sc) {
    expect_outcomes_match(legs[0][sc], legs[1][sc]);
  }
}

TEST_F(Erc20PermitStorageRefundTest, DeleteAllowanceNetsStorageAcrossRecreation)
{
  std::vector<std::vector<Step>> const scenarios{
      {permit_step<u8"increase_allowance_signed">(
           kOwner, {{.spender = kSpender, .amount = 500}}, SignedToken{}),
       erc20_step<u8"delete_allowance">(kOwner, kSpender),
       erc20_step<u8"allowance">(kOwner, kOwner, kSpender)},
      {permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 500}}, SignedToken{})},
      {erc20_step<u8"delete_allowance">(kOwner, kSpender),
       permit_step<u8"increase_allowance_signed">(
           kOwner, {{.spender = kSpender, .amount = 7}}, SignedToken{}),
       erc20_step<u8"allowance">(kOwner, kOwner, kSpender)},
  };
  std::vector<std::vector<LegOutcome>> legs;
  for (auto token : {kNativeToken, kRv32Token}) {
    SCOPED_TRACE(token == kNativeToken ? "native" : "rv32");
    fund(token, kActorFunds);
    std::vector<int128_t> balances;
    legs.push_back(
        run_recording_balance(token, scenarios, {{0}, {1}, {2}}, balances));
    auto const &out = legs.back();
    ASSERT_EQ(balances.size(), 4U);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(balances[1], balances[0]) << "create and delete leave no entry";
    EXPECT_EQ(balances[1] - balances[2], entry_cost());
    EXPECT_EQ(balances[3], balances[2])
        << "delete and regrant retain one entry";
    for (auto const &outcome : out) {
      EXPECT_EQ(outcome.commit, ExecResult::ExecResultOK);
    }
    ASSERT_EQ(out[0].records.size(), 3U);
    EXPECT_EQ(out[0].records[0], ok());
    EXPECT_EQ(out[0].records[1], ok());
    EXPECT_EQ(out[0].records[2], ok_amount(0));
    ASSERT_EQ(out[1].records.size(), 1U);
    EXPECT_EQ(out[1].records[0], ok());
    ASSERT_EQ(out[2].records.size(), 3U);
    EXPECT_EQ(out[2].records[0], ok());
    EXPECT_EQ(out[2].records[1], ok());
    EXPECT_EQ(out[2].records[2], ok_amount(7));
  }
  for (size_t sc = 0; sc < scenarios.size(); ++sc) {
    expect_outcomes_match(legs[0][sc], legs[1][sc]);
  }
}

// The refund is usable in the transaction that deletes: with the token
// contract unable to pay for a second entry, a new allowance fails alone but
// commits after a delete in the same transaction. This is what the exclusive
// lock on the deletion buys; an unlocked delete refunds only at commit.
TEST_F(Erc20PermitStorageRefundTest, DeleteAllowanceSignedRefundIsUsableInTheTx)
{
  std::vector<std::vector<Step>> const scenarios{
      {permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kSpender, .amount = 500}}, SignedToken{})},
      {permit_step<u8"increase_allowance_signed">(
          kOwner, {{.spender = kThirdParty, .amount = 70}}, SignedToken{})},
      {
          permit_step<u8"delete_allowance_signed">(
              kOwner, {{.spender = kSpender, .amount = 0, .deletion = true}},
              SignedToken{}),
          permit_step<u8"increase_allowance_signed">(
              kOwner, {{.spender = kThirdParty, .amount = 70}}, SignedToken{}),
          erc20_step<u8"allowance">(kOwner, kOwner, kSpender),
          erc20_step<u8"allowance">(kOwner, kOwner, kThirdParty),
      },
  };
  // One short of paying for two entries once setup's writes are paid for.
  int128_t const headroom = (2 * entry_cost()) - 1;

  std::vector<std::vector<LegOutcome>> legs;
  for (auto token : {kNativeToken, kRv32Token}) {
    SCOPED_TRACE(token == kNativeToken ? "native" : "rv32");
    // Setup writes the total supply; the RV32 reference also writes its owner
    // record and the owner's balance.
    int128_t setup_cost = entry_cost();
    if (token == kRv32Token) {
      setup_cost += get_default_cost_model()->get_bytestring_cost(
                        sizeof(ContractAddress)) +
                    entry_cost();
    }
    fund(token, setup_cost + headroom);

    std::vector<int128_t> balances;
    legs.push_back(
        run_recording_balance(token, scenarios, {{0}, {1}, {2}}, balances));
    auto const &out = legs.back();
    ASSERT_EQ(balances.size(), 4U);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(balances[0], headroom) << "setup cost other than expected";

    EXPECT_EQ(out[0].commit, ExecResult::ExecResultOK);
    EXPECT_EQ(balances[1], entry_cost() - 1);

    EXPECT_NE(out[1].commit, ExecResult::ExecResultOK)
        << "a second entry committed without the storage to pay for it";
    EXPECT_EQ(balances[2], entry_cost() - 1);

    EXPECT_EQ(out[2].commit, ExecResult::ExecResultOK)
        << "the delete's refund was not usable in its own transaction";
    ASSERT_EQ(out[2].records.size(), 4U);
    EXPECT_EQ(out[2].records[0], ok());
    EXPECT_EQ(out[2].records[1], ok());
    EXPECT_EQ(out[2].records[2], ok_amount(0));
    EXPECT_EQ(out[2].records[3], ok_amount(70));
    EXPECT_EQ(balances[3], entry_cost() - 1);
  }
  for (size_t sc = 0; sc < scenarios.size(); ++sc) {
    expect_outcomes_match(legs[0][sc], legs[1][sc]);
  }
}

// SignedToken is the C++ mirror of `Signed<T>`: no bytes on the wire, its
// handle assigned out-of-band.
TEST(SignedTokenCodec, EncodesAsZeroBytes)
{
  auto encoded = x23::serialize_cxs(SignedToken{.handle = 7});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(encoded->empty());
  EXPECT_TRUE(x23::serialize(SignedToken{.handle = 7}).empty());

  SignedToken decoded;
  EXPECT_TRUE(x23::deserialize_cxs(&decoded, std::span<uint8_t const>{}));
  EXPECT_EQ(decoded.handle, SignedToken::kUnassigned);

  // Only the non-Signed arguments reach the wire: the same bytes as a
  // transfer(receiver, amount).
  auto calldata =
      permit_meta::make_calldata<u8"transfer_from_signed">(SignedToken{},
                                                           ContractAddress{},
                                                           make_amount(7));
  EXPECT_EQ(calldata, erc20_meta::make_calldata<u8"transfer">(ContractAddress{},
                                                              make_amount(7)));
}

// NOLINTEND(bugprone-unchecked-optional-access)
// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
