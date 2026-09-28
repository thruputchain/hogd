#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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

//
// Conformance tests for the native token's ERC20Permit surface
// (groundhog-erc20-permit-if): each scenario runs once against the native
// token precompile and once against the RV32 reference `itc_erc20_permit`
// (itc_erc20 with ERC20Permit on top), with the same actors at the same
// addresses, and requires the two to agree byte-for-byte on the commit result,
// on every call's return or abort bytes, and on the emitted events (modulo the
// emitting contract address).
//
// The owner is a native driver that mints real PermitGrant auth tokens
// through kMintAuthToken, so the grant's minter -- the address the token
// contract treats as the owner -- is supplied by the runtime exactly as it is
// for a wallet's grant. A step either calls the token directly from the owner
// or has the owner forward the grants' handles to a relay at another actor,
// which calls the token with them.
//

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

// A captured call outcome: a tag byte, the child frame status, and the
// literal bytes the callee returned or aborted with (see append_invoke_record).
using Record = std::vector<uint8_t>;

std::tuple<uint64_t, uint64_t>
make_amount(uint128_t amount)
{
  return std::make_tuple(static_cast<uint64_t>(amount >> 64),
                         static_cast<uint64_t>(amount));
}

// One auth token the owner mints in a step.
struct GrantSpec {
  ContractAddress spender;
  uint128_t amount;
  // The consumer the token names; the token under test when unset.
  std::optional<ContractAddress> consumer = std::nullopt;
  // Replaces the CXS-encoded PermitGrant payload when set.
  std::optional<std::vector<uint8_t>> payload = std::nullopt;
  // Child tokens attached to the grant's node.
  uint32_t child_count = 0;
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
  return abort_bytes(0xb19947acb2fa14ffULL, error);
}

Record
permit_abort(itc_erc20_permit::ERC20PermitError error)
{
  return abort_bytes(0xe8abb072f5171148ULL, error);
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
  Erc20PermitParityTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, /*empty_genesis=*/true,
          /*genesis_cost_model=*/zero_cost_model_xdr())
  {}

protected:
  // Actors, identical across the two legs so the only legitimate difference
  // between them is the token contract they call.
  static constexpr ContractAddress kOwner = make_contract_address(201);
  static constexpr ContractAddress kSpender = make_contract_address(202);
  static constexpr ContractAddress kThirdParty = make_contract_address(203);
  static constexpr ContractAddress kReceiver = make_contract_address(204);
  static constexpr ContractAddress kOtherContract = make_contract_address(205);
  static constexpr ContractAddress kRv32Token = make_contract_address(206);
  static constexpr ContractAddress kNativeToken = kNativeTokenContractAddress;

  static constexpr uint128_t kOwnerFunds = 1'000'000;
  static constexpr uint128_t kMaxInt128 =
      static_cast<uint128_t>(std::numeric_limits<int128_t>::max());

  static constexpr int32_t kDriverId = 60;

  // Driver method ids: the scenario (rooted at the owner), the setup calls
  // (rooted at the genesis root), and one relay per step.
  static constexpr Runtime::MethodID kScenarioMethod = 0;
  static constexpr Runtime::MethodID kSetupInitializeMethod = 1;
  static constexpr Runtime::MethodID kSetupMintMethod = 2;
  static constexpr Runtime::MethodID kRelayBase = 100;

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
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token);
    write_deployment(kNativeToken, native_token);

    auto driver = make_native_contract(kDriverId);
    write_contract(driver);
    for (auto const &addr :
         {kOwner, kSpender, kThirdParty, kGenesisRootContractAddress}) {
      write_deployment(addr, driver);
    }
  }

  // Invokes `method` on `target` forwarding `handles`, and appends the outcome
  // to `record`: 0x00, the child frame status and its literal return/abort
  // bytes when a child frame ran; 0x01 and the syscall error when the invoke
  // was rejected before one did; 0xEE and the host error otherwise.
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
      record.push_back(0xEE);
      record.push_back(static_cast<uint8_t>(r.error().code()));
      co_return;
    }
    if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      record.push_back(0x01);
      record.push_back(static_cast<uint8_t>(r.value()));
      co_return;
    }
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
      auto encoded = x23::serialize_cxs(
          itc_erc20_permit::PermitGrant{.spender = spec.spender,
                                        .amount = to_xdr(spec.amount)});
      if (!encoded) {
        ADD_FAILURE() << "could not encode a PermitGrant";
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

  static StorageKeyContractData step_key(size_t i)
  {
    return make_storage_key("permit_step_" + std::to_string(i));
  }

  // The driver contract for one leg: every method's token calls target
  // `token`.
  static std::unique_ptr<NativeContract> make_driver(ContractAddress token,
                                                     std::vector<Step> steps)
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

    for (size_t i = 0; i < steps.size(); ++i) {
      // The relay: forward whatever handles the owner passed down.
      fns.emplace(kRelayBase + i,
                  [token, step = steps[i]](NativeSyscallContext &ctx)
                      -> x23::Async<InvokeStatus<void>> {
                    auto parent =
                        ctx.handler().get_parent_supplied_auth_tokens();
                    Record record;
                    co_await append_invoke_record(
                        ctx, token, step.method, step.calldata,
                        std::vector<uint32_t>(parent.begin(), parent.end()),
                        record);
                    ctx.handler().set_return_buffer(ReturnBuffer(record.begin(),
                                                                 record.end()),
                                                    false, {});
                    co_return InvokeStatus<void>{};
                  });
    }

    fns.emplace(kScenarioMethod,
                [token, steps = std::move(steps)](NativeSyscallContext &ctx)
                    -> x23::Async<InvokeStatus<void>> {
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
                                                    step.calldata, minted[i],
                                                    record);
                    }
                    else {
                      co_await append_invoke_record(ctx, step.caller,
                                                    kRelayBase + i, {},
                                                    minted[i], record);
                    }

                    auto set = co_await ctx.syscall<SyscallNo::kBytestringSet>(
                        step_key(i),
                        std::as_bytes(std::span(record.data(), record.size())));
                    if (!set.has_value() ||
                        *set != SyscallErrorCode::kSyscallErrorCodeNone) {
                      ADD_FAILURE() << "could not persist step " << i;
                      co_return InvokeError{InvokeErrorCode::Deterministic};
                    }
                  }
                  co_return InvokeStatus<void>{};
                });

    return std::make_unique<NativeContract>(std::move(fns));
  }

  // Commits a transaction rooted at `root` calling the driver's `method`.
  // Returns the commit result, and the transaction's id for its event log.
  std::pair<ExecResult, EphemeralTxIdentifier>
  run_driver_tx(RuntimeContext &ctx, ContractAddress root,
                Runtime::MethodID method)
  {
    EphemeralTxIdentifier tx_id;
    SignedTransaction tx{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = root,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(tx.tx, ctx, &tx_id);
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    ExecResult commit = handler.try_commit_transaction()();
    if (commit == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                      ProposedTransaction{.signed_tx = tx})());
    }
    advance_block();
    return {commit, tx_id};
  }

  Record read_record(size_t i)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = step_key(i);
    auto item = lookup_storage_item_from_db(
        AddressAndKey{.address = kOwner, .key = std::move(key)});
    EXPECT_TRUE(item.has_value()) << "no record for step " << i;
    if (!item.has_value()) {
      return {};
    }
    auto const &bs = item->bytestring();
    return Record(bs.begin(), bs.end());
  }

  // Runs `steps` against `token`: initializes it (the RV32 reference only)
  // and funds the owner from the genesis root, then runs the scenario rooted
  // at the owner.
  LegOutcome run_leg(ContractAddress token, std::vector<Step> const &steps)
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(kDriverId, make_driver(token, steps));
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
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

    auto [commit, tx_id] = run_driver_tx(ctx, kOwner, kScenarioMethod);
    LegOutcome out{.commit = commit, .records = {}, .events = {}};
    if (commit == ExecResult::ExecResultOK) {
      for (size_t i = 0; i < steps.size(); ++i) {
        out.records.push_back(read_record(i));
      }
      auto log = get_tx_event_log(tx_id)();
      EXPECT_TRUE(log.has_value());
      if (log.has_value()) {
        out.events.assign(log->events.begin(), log->events.end());
      }
    }
    return out;
  }

  // Runs `steps` on both backends, requires byte parity, and returns the
  // native leg's per-step records with any relay framing removed, so a test
  // can pin what each step did.
  std::vector<Record> expect_parity(std::vector<Step> const &steps)
  {
    auto native = run_leg(kNativeToken, steps);
    auto rv32 = run_leg(kRv32Token, steps);

    EXPECT_EQ(native.commit, ExecResult::ExecResultOK);
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
  static Record ok_amount(uint128_t amount)
  {
    auto buf =
        Serializer<SerializationFormat::kCxs>::serialize(make_amount(amount));
    EXPECT_TRUE(buf.has_value());
    return returned(Record(buf->begin(), buf->end()));
  }

  std::vector<TxEmittedEvent> native_events_;
};

// A permit writes an allowance the spender then draws on through the ordinary
// transfer_from, exactly as an adjust_allowance by the owner would.
TEST_F(Erc20PermitParityTest, PermitWritesAllowanceSpentByTransferFrom)
{
  auto results = expect_parity({
      permit_step<u8"permit">(kSpender, {{.spender = kSpender, .amount = 500}},
                              SignedToken{}),
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
  // Approval for the permit, Transfer for the one transfer_from that landed.
  EXPECT_EQ(native_events_.size(), 2U);
}

// Anyone may submit a permit; the owner is the grant's minter, never the
// caller. Here the third party submits, and the owner submits its own.
TEST_F(Erc20PermitParityTest, PermitOwnerIsMinterWhoeverSubmits)
{
  auto results = expect_parity({
      permit_step<u8"permit">(kThirdParty,
                              {{.spender = kSpender, .amount = 70}},
                              SignedToken{}),
      permit_step<u8"permit">(kOwner, {{.spender = kSpender, .amount = 30}},
                              SignedToken{}),
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

// transfer_from_signed moves the granted amount in the one call and leaves no
// allowance behind.
TEST_F(Erc20PermitParityTest, TransferFromSignedMovesWithoutAllowance)
{
  auto results = expect_parity({
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 400}}, SignedToken{},
          kReceiver),
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
          kThirdParty),
      permit_step<u8"transfer_from_signed">(
          kOwner, {{.spender = kSpender, .amount = 10}}, SignedToken{},
          kReceiver),
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
          SignedToken{}, kReceiver),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = kOwnerFunds}},
          SignedToken{}, kReceiver),
      erc20_step<u8"get_balance_of">(kSpender, kReceiver),
  });
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0],
            aborted(erc20_abort(itc_erc20_permit::ERC20Error::TooLarge)));
  EXPECT_EQ(results[1], ok());
  EXPECT_EQ(results[2], ok_amount(kOwnerFunds));
}

// A granted amount a signed delta cannot carry is TooLarge on both paths.
TEST_F(Erc20PermitParityTest, GrantAboveInt128MaxIsTooLarge)
{
  constexpr uint128_t kHuge = kMaxInt128 + 1;
  auto results = expect_parity({
      permit_step<u8"permit">(kSpender,
                              {{.spender = kSpender, .amount = kHuge}},
                              SignedToken{}),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = kHuge}}, SignedToken{},
          kReceiver),
      permit_step<u8"permit">(kSpender,
                              {{.spender = kSpender, .amount = kMaxInt128}},
                              SignedToken{}),
  });
  ASSERT_EQ(results.size(), 3U);
  auto too_large = aborted(erc20_abort(itc_erc20_permit::ERC20Error::TooLarge));
  EXPECT_EQ(results[0], too_large);
  EXPECT_EQ(results[1], too_large);
  // INT128_MAX itself is a representable allowance.
  EXPECT_EQ(results[2], ok());
}

// A grant naming its own minter as spender: a self-allowance is rejected as
// adjust_allowance rejects it, while moving one's own tokens needs no
// allowance and goes through.
TEST_F(Erc20PermitParityTest, GrantToSelf)
{
  auto results = expect_parity({
      permit_step<u8"permit">(kOwner, {{.spender = kOwner, .amount = 5}},
                              SignedToken{}),
      permit_step<u8"transfer_from_signed">(kOwner,
                                            {{.spender = kOwner, .amount = 5}},
                                            SignedToken{}, kReceiver),
  });
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0],
            aborted(erc20_abort(itc_erc20_permit::ERC20Error::Internal)));
  EXPECT_EQ(results[1], ok());
}

// A grant minted for another consumer reaches the token (the handle is the
// caller's to forward) but the runtime refuses to let it be consumed there.
// kConsumeAuthToken is syscall 32, the first number whose CXS encoding differs
// between a signed and an unsigned integer, so this also pins that the native
// LibraryExn::Syscall abort encodes the syscall number as the SDK's u32 does.
TEST_F(Erc20PermitParityTest, GrantForAnotherConsumerIsNotConsumable)
{
  auto results = expect_parity({
      permit_step<u8"permit">(
          kSpender,
          {{.spender = kSpender, .amount = 5, .consumer = kOtherContract}},
          SignedToken{}),
      permit_step<u8"transfer_from_signed">(
          kSpender,
          {{.spender = kSpender, .amount = 5, .consumer = kOtherContract}},
          SignedToken{}, kReceiver),
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
      permit_step<u8"permit">(kSpender, {}, SignedToken{}),
      permit_step<u8"permit">(kSpender,
                              {{.spender = kSpender, .amount = 1},
                               {.spender = kSpender, .amount = 2}},
                              SignedToken{}),
      permit_step<u8"transfer_from_signed">(kSpender, {}, SignedToken{},
                                            kReceiver),
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
                                    .amount = to_xdr(uint128_t{5})});
  ASSERT_TRUE(valid.has_value());
  std::vector<uint8_t> trailing(valid->begin(), valid->end());
  trailing.push_back(0);
  std::vector<uint8_t> truncated(valid->begin(), valid->end() - 1);

  auto results = expect_parity({
      permit_step<u8"permit">(
          kSpender, {{.spender = kSpender, .amount = 5, .payload = trailing}},
          SignedToken{}),
      permit_step<u8"permit">(
          kSpender, {{.spender = kSpender, .amount = 5, .payload = truncated}},
          SignedToken{}),
      permit_step<u8"permit">(kSpender,
                              {{.spender = kSpender,
                                .amount = 5,
                                .payload = std::vector<uint8_t>{}}},
                              SignedToken{}),
      permit_step<u8"permit">(
          kSpender, {{.spender = kSpender, .amount = 5, .child_count = 1}},
          SignedToken{}),
      permit_step<u8"transfer_from_signed">(
          kSpender, {{.spender = kSpender, .amount = 5, .child_count = 2}},
          SignedToken{}, kReceiver),
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
      permit_step<u8"permit">(kOwner, {{.spender = kSpender, .amount = 9}},
                              SignedToken{}),
      Step{.caller = kOwner,
           .grants = {},
           .method = permit_meta::method_id<u8"permit">(),
           .calldata = permit_meta::make_calldata<u8"permit">(SignedToken{}),
           .reuse_grants_of = 0},
      erc20_step<u8"allowance">(kSpender, kOwner, kSpender),
  });
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0], ok());
  ASSERT_FALSE(results[1].empty());
  EXPECT_EQ(results[1][0], 0x01) << "the replay reached a token frame";
  EXPECT_EQ(results[2], ok_amount(9));
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

  auto calldata =
      permit_meta::make_calldata<u8"transfer_from_signed">(SignedToken{},
                                                           ContractAddress{});
  EXPECT_EQ(calldata.size(), sizeof(ContractAddress));
}

// NOLINTEND(bugprone-unchecked-optional-access)
// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
