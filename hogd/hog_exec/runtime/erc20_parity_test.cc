#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/itc_erc20.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

// All storage costs are zeroed for these tests. The native HOG precompile and
// the RV32 itc_erc20 charge storage differently (native balance_cost vs plain
// nnint_cost on a contract-data key), and that difference is deliberately out
// of scope here: parity is about ERC20 *semantics*, not storage-cost
// accounting. Zeroing costs keeps a divergence in storage pricing from showing
// up as a spurious commit-time difference between the two backends.
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

using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

// A captured ERC20 call outcome: a tag byte, the child frame status, and the
// literal bytes the callee returned or aborted with. Comparing this whole
// record byte-for-byte across the two backends covers both the return path
// (the encoded Result<void, ERC20Error> variant) and the abort path (the
// literal bytes a manual revert / fault leaves behind).
using CapturedRecord = std::vector<uint8_t>;

// The itc_erc20 error carried by a decoded contract Result, or nullopt when
// the result is Ok or the abort payload was not an itc_erc20 exception (a
// library exception, or a body that did not parse -- ContractErrorType keeps
// those as distinct variant alternatives). Matching on the payload type
// rather than on a hard-coded selector keeps this honest if the generated
// exception ever changes shape.
template<typename ResultT>
std::optional<itc_erc20::ERC20Error>
erc20_error_of(ResultT const &result)
{
  if (result.is_ok()) {
    return std::nullopt;
  }
  return std::visit(
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
}

// The itc_erc20 error a captured step aborted with. Returns nullopt unless
// the step ran a child frame that aborted with a decodable itc_erc20
// exception -- so callers pin *which* error a rejection reported, not merely
// that the frame aborted.
template<StringLiteral Name>
std::optional<itc_erc20::ERC20Error>
step_erc20_error(CapturedRecord const &record)
{
  // [0] is the capture tag (0x00 == a child frame ran), [1] the frame status,
  // and the rest is the literal return/abort payload.
  if (record.size() < 2 || record[0] != 0x00 ||
      static_cast<InvocationFrameStatusCode>(record[1]) !=
          InvocationFrameStatusCode::Abort) {
    return std::nullopt;
  }
  auto payload = std::as_bytes(std::span(record.data() + 2, record.size() - 2));
  auto decoded =
      erc20_meta::decode_return<Name>(payload, /*manually_reverted=*/true);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  return erc20_error_of(*decoded);
}

// The amount a captured step's child frame returned, or nullopt unless the
// step ran a child frame that returned a decodable Ok(amount_t). Lets tests
// pin the numeric value a total_supply / get_balance_of step read, on top of
// the cross-backend byte comparison.
template<StringLiteral Name>
std::optional<uint128_t>
step_ok_amount(CapturedRecord const &record)
{
  if (record.size() < 2 || record[0] != 0x00 ||
      static_cast<InvocationFrameStatusCode>(record[1]) !=
          InvocationFrameStatusCode::Return) {
    return std::nullopt;
  }
  auto payload = std::as_bytes(std::span(record.data() + 2, record.size() - 2));
  auto decoded =
      erc20_meta::decode_return<Name>(payload, /*manually_reverted=*/false);
  if (!decoded.has_value() || !decoded->is_ok()) {
    return std::nullopt;
  }
  auto const &[hi, lo] = decoded->ok();
  return (uint128_t{hi} << 64) | lo;
}

} // namespace

// Drives the native HOG precompile and the RV32 itc_erc20 contract through the
// same ERC20 operations, capturing each call's commit ExecResult plus the
// literal return/abort bytes. The two backends agree byte-for-byte on valid
// operations, on allowance handling, and on balance checking: the native
// token's transfer and transfer_from both enforce a strict per-debit check at
// invoke, just like itc_erc20, returning ERC20Error::TooLarge when
// amount > available. The *Parity tests guard all of these behaviors, including
// boundary cases (exact-balance succeeds, one-over aborts) and the reference
// check order (allowance debit first, then owner debit, then receiver credit)
// for transfer_from.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class Erc20ParityTest : public ::testing::Test,
                        public NativeContractTestFramework {
public:
  Erc20ParityTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, /*empty_genesis=*/true,
          /*genesis_cost_model=*/zero_cost_model_xdr())
  {}

protected:
  // Addresses are fixed per test; the native precompile lives at the genesis
  // preset address, everything else is arbitrary.
  static constexpr ContractAddress kNativeTokenAddr =
      kNativeTokenContractAddress;
  static constexpr ContractAddress kRv32TokenAddr = make_contract_address(200);
  static constexpr ContractAddress kRv32AdminAddr = make_contract_address(201);
  static constexpr ContractAddress kNativeCallerAddr =
      make_contract_address(202);
  static constexpr ContractAddress kRv32CallerAddr = make_contract_address(203);
  // Distinct (non-self) recipients for the baseline transfer.
  static constexpr ContractAddress kNativeRecipientAddr =
      make_contract_address(204);
  static constexpr ContractAddress kRv32RecipientAddr =
      make_contract_address(205);
  // transfer_from: an owner (token holder, allowance granter) and a spender
  // (the delegated caller), per backend.
  static constexpr ContractAddress kNativeOwnerAddr =
      make_contract_address(206);
  static constexpr ContractAddress kNativeSpenderAddr =
      make_contract_address(207);
  static constexpr ContractAddress kRv32OwnerAddr = make_contract_address(208);
  static constexpr ContractAddress kRv32SpenderAddr =
      make_contract_address(209);
  // transfer_from to a distinct recipient: two extra recipient addresses.
  static constexpr ContractAddress kNativeTfRecipientAddr =
      make_contract_address(210);
  static constexpr ContractAddress kRv32TfRecipientAddr =
      make_contract_address(211);

  // Native contract ids in the multiplexed context's native map.
  static constexpr int32_t kRv32AdminId = 50;
  static constexpr int32_t kNativeCallerId = 51;
  static constexpr int32_t kRv32CallerId = 52;
  static constexpr int32_t kNativeOwnerId = 53;
  static constexpr int32_t kNativeSpenderId = 54;
  static constexpr int32_t kRv32OwnerId = 55;
  static constexpr int32_t kRv32SpenderId = 56;
  // The mint/burn capture driver (deployed at the genesis root address, or at
  // an ordinary caller address for the unauthorized-caller tests).
  static constexpr int32_t kMintBurnDriverId = 57;

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  static std::tuple<int64_t, uint64_t> make_difference(int128_t amount)
  {
    return std::make_tuple(static_cast<int64_t>(amount >> 64),
                           static_cast<uint64_t>(amount &
                                                 0xffffffffffffffffULL));
  }

  // Compute the contract-data storage key that itc_erc20 uses for addr's
  // balance. The RV32 SDK's .hash() CXS-encodes the tuple (b"balance", addr)
  // and SHA-256s the result. CXS fixed-opaque arrays have no padding, so the
  // preimage is exactly: 7 bytes of "balance" || 32 bytes of addr = 39 bytes.
  static StorageKeyContractData
  itc_erc20_balance_key(ContractAddress const &addr)
  {
    std::array<uint8_t, 39> preimage{};
    // "balance" in ASCII/UTF-8
    static constexpr std::array<uint8_t, 7> kTag = {0x62, 0x61, 0x6c, 0x61,
                                                    0x6e, 0x63, 0x65};
    std::ranges::copy(kTag, preimage.begin());
    std::ranges::copy(addr, preimage.begin() + 7);
    return StorageKeyContractData{
        sha256(std::span<const uint8_t>(preimage.data(), preimage.size()))};
  }

  // Appends one ERC20 call's outcome to `record`: a tag byte, then either the
  // error code that stopped the call, or the child frame status followed by
  // the literal bytes the callee returned or aborted with.
  static void append_invoke_record(NativeSyscallContext &ctx,
                                   ContractAddress const &target,
                                   Runtime::MethodID method_id,
                                   Calldata const &calldata,
                                   std::vector<uint8_t> &record)
  {
    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{
        .entrypoint_id = method_id,
        .gas_limit = static_cast<GasAmount>(UINT64_MAX),
        .flags = std::to_underlying(InvokeFlag::None),
    };
    auto r = ctx.syscall<SyscallNo::kInvoke>(
        target, std::as_bytes(std::span(calldata.data(), calldata.size())),
        std::span<uint32_t>{}, inputs, outputs);

    if (!r.has_value()) {
      // Host-level (non-deterministic) failure: should not happen on these
      // paths, but record it distinctly rather than hiding it.
      record.push_back(0xEE);
      record.push_back(static_cast<uint8_t>(r.error().code()));
      return;
    }
    if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      // Invoke rejected before any child frame ran.
      record.push_back(0x01);
      record.push_back(static_cast<uint8_t>(r.value()));
      return;
    }
    record.push_back(0x00);
    record.push_back(static_cast<uint8_t>(outputs.frame_status));
    auto rb = ctx.handler().get_return_buffer();
    if (rb.has_value() && rb.value().has_value()) {
      auto const *buf = rb.value().value();
      for (auto byte_val : *buf) {
        record.push_back(static_cast<uint8_t>(byte_val));
      }
    }
    else {
      // Fault: frame ran but produced no return buffer.
      record.push_back(0xFF);
    }
  }

  // Writes a capture driver's record to `result_key` so the test can read it
  // back from the DB.
  static InvokeStatus<void>
  persist_record(NativeSyscallContext &ctx,
                 StorageKeyContractData const &result_key,
                 std::vector<uint8_t> const &record)
  {
    auto set_r = ctx.syscall<SyscallNo::kBytestringSet>(
        result_key, std::as_bytes(std::span(record.data(), record.size())),
        std::to_underlying(StorageDeltaFlags::None));
    if (!set_r.has_value() ||
        set_r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "capture driver failed to persist result";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  }

  // Builds a native "driver" contract whose method 0 invokes `target`'s ERC20
  // `Name` entrypoint with `args`, then records (tag, frame_status, literal
  // return bytes) into `result_key` so the test can read it back from the DB.
  template<StringLiteral Name, typename... Args>
  NativeContract::function_t
  make_capture_driver(ContractAddress target, StorageKeyContractData result_key,
                      Args... args)
  {
    auto calldata = erc20_meta::make_calldata<Name>(args...);
    Runtime::MethodID method_id = erc20_meta::method_id<Name>();
    return [target, result_key, method_id, calldata = std::move(calldata)](
               NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
      std::vector<uint8_t> record;
      append_invoke_record(ctx, target, method_id, calldata, record);
      return persist_record(ctx, result_key, record);
    };
  }

  // One ERC20 call in a multi-step capture driver: which contract to call,
  // with what calldata, and the key its outcome record is written under.
  struct CaptureStep {
    ContractAddress target;
    Runtime::MethodID method_id;
    Calldata calldata;
    StorageKeyContractData result_key;
  };

  // Builds a CaptureStep invoking `target`'s ERC20 `Name` entrypoint.
  template<StringLiteral Name, typename... Args>
  static CaptureStep make_step(ContractAddress target,
                               StorageKeyContractData result_key, Args... args)
  {
    return CaptureStep{.target = target,
                       .method_id = erc20_meta::method_id<Name>(),
                       .calldata = erc20_meta::make_calldata<Name>(args...),
                       .result_key = result_key};
  }

  // Builds a native driver whose method 0 runs every step in order from a
  // single root frame -- the shape the netting findings on hogd#1918 rely on:
  // a call that must be rejected, followed by a sibling call whose opposite
  // delta on the same NNInt key would net it away before commit. Each step
  // records its own outcome, and the driver always returns success, so a step
  // that aborts neither tears down the transaction nor hides the steps after
  // it.
  static NativeContract::function_t
  make_multi_capture_driver(std::vector<CaptureStep> steps)
  {
    return [steps = std::move(steps)](
               NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
      for (auto const &step : steps) {
        std::vector<uint8_t> record;
        append_invoke_record(ctx, step.target, step.method_id, step.calldata,
                             record);
        if (auto r = persist_record(ctx, step.result_key, record);
            !r.has_value()) {
          return r.error();
        }
      }
      return {};
    };
  }

  // Reads back a record persisted by a capture driver.
  CapturedRecord read_record(ContractAddress const &driver_addr,
                             StorageKeyContractData const &result_key)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = result_key;
    auto item = lookup_storage_item_from_db(
        AddressAndKey{.address = driver_addr, .key = std::move(key)});
    EXPECT_TRUE(item.has_value()) << "no captured record found";
    CapturedRecord out;
    if (item.has_value()) {
      auto const &bs = item->bytestring();
      out.assign(bs.begin(), bs.end());
    }
    return out;
  }

  void run_tx(ContractAddress const &addr, Runtime::MethodID method_id,
              RuntimeContext &ctx)
  {
    auto handler_tx = SignedTransaction{
        .tx =
            Transaction{.invocation =
                            Invocation{.invoked_address = addr,
                                       .entrypoint =
                                           entrypoint_from_method_id(method_id),
                                       .gas_limit = UINT64_MAX,
                                       .calldata = {}},
                        .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(handler_tx, ctx);
    ASSERT_TRUE(handler.root_invoke().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    ASSERT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
    advance_block();
  }

  // Runs `addr`'s method-0 capture driver and returns a single byte-comparable
  // outcome: the commit ExecResult byte followed by the literal invoke record
  // (tag + child frame status + child return/abort bytes). The record is only
  // appended when the commit succeeds -- a failed commit rolls back the
  // driver's storage write, and that rollback is itself part of the compared
  // outcome (the deferred-reject-at-commit vs strict-reject-at-invoke split).
  // The driver always returns success, so root_invoke always succeeds; the
  // divergence surfaces in the commit result and/or the captured bytes.
  CapturedRecord run_capture_tx(ContractAddress const &addr,
                                StorageKeyContractData const &result_key,
                                RuntimeContext &ctx)
  {
    auto handler_tx = SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = addr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(handler_tx, ctx);
    EXPECT_TRUE(handler.root_invoke().is_root_success());
    ExecResult commit = handler.try_commit_transaction();
    CapturedRecord outcome;
    outcome.push_back(static_cast<uint8_t>(commit));
    if (commit == ExecResult::ExecResultOK) {
      EXPECT_TRUE(
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = handler_tx})());
      advance_block();
      auto rec = read_record(addr, result_key);
      outcome.insert(outcome.end(), rec.begin(), rec.end());
    }
    else {
      advance_block();
    }
    return outcome;
  }

  // The comparable outcome of a multi-step capture transaction: the commit
  // ExecResult plus one record per step. A failed commit rolls the driver's
  // writes back, so the step records are only present when it succeeded --
  // the rollback is itself part of the compared outcome.
  using MultiStepOutcome = std::pair<ExecResult, std::vector<CapturedRecord>>;

  // Runs `addr`'s multi-step driver (its `method_id` entrypoint) and returns
  // its outcome. The driver always returns success, so root_invoke always
  // succeeds; a divergence surfaces in the commit result and/or the per-step
  // bytes.
  MultiStepOutcome
  run_multi_capture_tx(ContractAddress const &addr,
                       std::vector<StorageKeyContractData> const &result_keys,
                       RuntimeContext &ctx, Runtime::MethodID method_id = 0)
  {
    auto handler_tx = SignedTransaction{
        .tx =
            Transaction{.invocation =
                            Invocation{.invoked_address = addr,
                                       .entrypoint =
                                           entrypoint_from_method_id(method_id),
                                       .gas_limit = UINT64_MAX,
                                       .calldata = {}},
                        .uniquifier = make_new_uniquifier()}};
    auto handler = make_new_system_call_handler(handler_tx, ctx);
    EXPECT_TRUE(handler.root_invoke().is_root_success());
    ExecResult commit = handler.try_commit_transaction();
    std::vector<CapturedRecord> records;
    if (commit == ExecResult::ExecResultOK) {
      EXPECT_TRUE(
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = handler_tx})());
      advance_block();
      records.reserve(result_keys.size());
      for (auto const &result_key : result_keys) {
        records.push_back(read_record(addr, result_key));
      }
    }
    else {
      advance_block();
    }
    return {commit, records};
  }

  // Runs the allowance-netting sequence from hogd#1918 against both backends:
  // in ONE transaction a caller decrements a *fresh* (caller, spender)
  // allowance by `magnitude` and then, as a sibling call, increments it
  // straight back. Under the old deferred (non-strict) NNInt path the
  // decrement returned Ok and emitted an Approval event for a diff the
  // committed allowance could never fund; the sibling increment then netted
  // the pending add back to zero, so nothing was left for commit-time
  // validation to reject. With strict allowance updates the decrement must be
  // rejected at invoke, and the increment that follows it must be unaffected.
  //
  // Steps (recorded in order): adjust_allowance(spender, -magnitude),
  // adjust_allowance(spender, +magnitude), allowance(caller, spender),
  // allowance(caller, untouched_spender). The two trailing reads pin the
  // committed allowance without reaching into either backend's storage-key
  // derivation: the second one is a known zero, so the netted-away outcome
  // (both reads equal) is distinguishable from the correct one (they differ).
  std::pair<MultiStepOutcome, MultiStepOutcome>
  run_allowance_netting_parity(int128_t magnitude)
  {
    std::vector<StorageKeyContractData> native_keys{
        make_storage_key("native_step0"), make_storage_key("native_step1"),
        make_storage_key("native_step2"), make_storage_key("native_step3")};
    std::vector<StorageKeyContractData> rv32_keys{
        make_storage_key("rv32_step0"), make_storage_key("rv32_step1"),
        make_storage_key("rv32_step2"), make_storage_key("rv32_step3")};

    auto make_steps =
        [magnitude](ContractAddress token, ContractAddress caller,
                    ContractAddress spender, ContractAddress zero_spender,
                    std::vector<StorageKeyContractData> const &keys) {
          return std::vector<CaptureStep>{
              make_step<u8"adjust_allowance">(token, keys[0], spender,
                                              make_difference(-magnitude)),
              make_step<u8"adjust_allowance">(token, keys[1], spender,
                                              make_difference(magnitude)),
              make_step<u8"allowance">(token, keys[2], caller, spender),
              make_step<u8"allowance">(token, keys[3], caller, zero_spender)};
        };

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_caller_fns;
    native_caller_fns.emplace(0, make_multi_capture_driver(make_steps(
                                     kNativeTokenAddr, kNativeCallerAddr,
                                     kNativeSpenderAddr, kNativeRecipientAddr,
                                     native_keys)));
    auto native_caller =
        std::make_unique<NativeContract>(std::move(native_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_caller_fns;
    rv32_caller_fns.emplace(0, make_multi_capture_driver(
                                   make_steps(kRv32TokenAddr, kRv32CallerAddr,
                                              kRv32SpenderAddr,
                                              kRv32RecipientAddr, rv32_keys)));
    auto rv32_caller =
        std::make_unique<NativeContract>(std::move(rv32_caller_fns));

    // The RV32 admin just initializes itc_erc20 (no balances are involved).
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kRv32AdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kNativeCallerId, std::move(native_caller));
    native_map.emplace(kRv32CallerId, std::move(rv32_caller));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kRv32AdminAddr, kRv32AdminId);
    deploy_native(kNativeCallerAddr, kNativeCallerId);
    deploy_native(kRv32CallerAddr, kRv32CallerId);

    run_tx(kRv32AdminAddr, 0, ctx);

    return {run_multi_capture_tx(kNativeCallerAddr, native_keys, ctx),
            run_multi_capture_tx(kRv32CallerAddr, rv32_keys, ctx)};
  }

  // Runs the cross-child balance-netting sequence from hogd#1918 against both
  // backends: a caller holding NO tokens calls transfer(owner, amount) and
  // then, as a sibling call in the same transaction,
  // transfer_from(owner, caller, amount) against an allowance the owner
  // granted it beforehand. Under the old deferred (non-strict) NNInt path the
  // first call returned Ok and published a Transfer event the caller could not
  // fund; the sibling call's credit then repaired the caller's pending
  // negative balance through the parent-local item view, so the root commit
  // saw a valid final delta. With a strict per-debit check the unfunded
  // transfer must be rejected at invoke, while the delegated transfer that
  // follows it still succeeds on its own merits.
  //
  // Steps (recorded in order): transfer(owner, amount),
  // transfer_from(owner, caller, amount), get_balance_of(caller),
  // get_balance_of(owner), get_balance_of(untouched). The trailing reads pin
  // the committed balances; the last one is a known zero, so which of the two
  // parties ended up holding the tokens is decidable without depending on
  // either backend's return encoding.
  std::pair<MultiStepOutcome, MultiStepOutcome>
  run_unfunded_transfer_netting_parity(uint128_t amount)
  {
    std::vector<StorageKeyContractData> native_keys{
        make_storage_key("native_step0"), make_storage_key("native_step1"),
        make_storage_key("native_step2"), make_storage_key("native_step3"),
        make_storage_key("native_step4")};
    std::vector<StorageKeyContractData> rv32_keys{
        make_storage_key("rv32_step0"), make_storage_key("rv32_step1"),
        make_storage_key("rv32_step2"), make_storage_key("rv32_step3"),
        make_storage_key("rv32_step4")};

    auto make_steps =
        [amount](ContractAddress token, ContractAddress caller,
                 ContractAddress owner, ContractAddress zero_holder,
                 std::vector<StorageKeyContractData> const &keys) {
          return std::vector<CaptureStep>{
              make_step<u8"transfer">(token, keys[0], owner,
                                      make_amount(amount)),
              make_step<u8"transfer_from">(token, keys[1], owner, caller,
                                           make_amount(amount)),
              make_step<u8"get_balance_of">(token, keys[2], caller),
              make_step<u8"get_balance_of">(token, keys[3], owner),
              make_step<u8"get_balance_of">(token, keys[4], zero_holder)};
        };

    // An owner driver that grants `spender` an allowance on `token`.
    auto make_grant =
        [amount](ContractAddress token,
                 ContractAddress spender) -> NativeContract::function_t {
      return [token, spender, amount](
                 NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        ContractTraitClient<itc_erc20::ERC20> client(token);
        auto r = client.invoke_opt<u8"adjust_allowance">(
            ctx, spender, make_difference(static_cast<int128_t>(amount)));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "grant allowance failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      };
    };

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_caller_fns;
    native_caller_fns.emplace(
        0, make_multi_capture_driver(
               make_steps(kNativeTokenAddr, kNativeCallerAddr, kNativeOwnerAddr,
                          kNativeRecipientAddr, native_keys)));
    auto native_caller =
        std::make_unique<NativeContract>(std::move(native_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_caller_fns;
    rv32_caller_fns.emplace(0, make_multi_capture_driver(
                                   make_steps(kRv32TokenAddr, kRv32CallerAddr,
                                              kRv32OwnerAddr,
                                              kRv32RecipientAddr, rv32_keys)));
    auto rv32_caller =
        std::make_unique<NativeContract>(std::move(rv32_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_owner_fns;
    native_owner_fns.emplace(0,
                             make_grant(kNativeTokenAddr, kNativeCallerAddr));
    auto native_owner =
        std::make_unique<NativeContract>(std::move(native_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_owner_fns;
    rv32_owner_fns.emplace(0, make_grant(kRv32TokenAddr, kRv32CallerAddr));
    auto rv32_owner =
        std::make_unique<NativeContract>(std::move(rv32_owner_fns));

    // The RV32 admin initializes itc_erc20 and mints the owner's balance.
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0, [amount](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kRv32AdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kRv32OwnerAddr,
                                                  make_amount(amount));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kNativeCallerId, std::move(native_caller));
    native_map.emplace(kRv32CallerId, std::move(rv32_caller));
    native_map.emplace(kNativeOwnerId, std::move(native_owner));
    native_map.emplace(kRv32OwnerId, std::move(rv32_owner));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kRv32AdminAddr, kRv32AdminId);
    deploy_native(kNativeCallerAddr, kNativeCallerId);
    deploy_native(kRv32CallerAddr, kRv32CallerId);
    deploy_native(kNativeOwnerAddr, kNativeOwnerId);
    deploy_native(kRv32OwnerAddr, kRv32OwnerId);

    // Owner balances: a native HOG grant for the native owner, an admin mint
    // for the RV32 owner. The callers are funded on neither backend.
    setup_grant_storage_token(kNativeOwnerAddr, static_cast<int128_t>(amount));
    run_tx(kRv32AdminAddr, 0, ctx);

    // Owners grant the callers an allowance for exactly `amount`.
    run_tx(kNativeOwnerAddr, 0, ctx);
    run_tx(kRv32OwnerAddr, 0, ctx);

    return {run_multi_capture_tx(kNativeCallerAddr, native_keys, ctx),
            run_multi_capture_tx(kRv32CallerAddr, rv32_keys, ctx)};
  }

  // Runs `transfer(receiver, transfer_amount)` against both the native
  // precompile and the RV32 itc_erc20, with each caller starting from a
  // freshly-minted balance of `start_balance`. Pass each caller's own address
  // as the receiver for a self-transfer, or a distinct address for an ordinary
  // transfer. When recipient_start_balance > 0 the receiver is pre-funded on
  // both backends before the transfer runs (native: direct write bypassing the
  // supply-cap guard; RV32: direct write to the itc_erc20 balance key).
  // Returns {native_record, rv32_record}, the byte-for-byte captured outcomes
  // (tag + frame status + literal return/abort bytes).
  std::pair<CapturedRecord, CapturedRecord>
  run_transfer_parity(uint128_t start_balance, uint128_t transfer_amount,
                      ContractAddress native_receiver,
                      ContractAddress rv32_receiver,
                      uint128_t recipient_start_balance = 0)
  {
    auto native_result_key = make_storage_key("native_result");
    auto rv32_result_key = make_storage_key("rv32_result");

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_caller_fns;
    native_caller_fns.emplace(
        0, make_capture_driver<u8"transfer">(kNativeTokenAddr,
                                             native_result_key, native_receiver,
                                             make_amount(transfer_amount)));
    auto native_caller =
        std::make_unique<NativeContract>(std::move(native_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_caller_fns;
    rv32_caller_fns.emplace(
        0, make_capture_driver<u8"transfer">(kRv32TokenAddr, rv32_result_key,
                                             rv32_receiver,
                                             make_amount(transfer_amount)));
    auto rv32_caller =
        std::make_unique<NativeContract>(std::move(rv32_caller_fns));

    // The RV32 admin initializes itc_erc20 (as its own-address owner) and mints
    // the caller's starting balance.
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0,
        [start_balance](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kRv32AdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kRv32CallerAddr,
                                                  make_amount(start_balance));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kNativeCallerId, std::move(native_caller));
    native_map.emplace(kRv32CallerId, std::move(rv32_caller));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto admin_obj = make_native_contract(kRv32AdminId);
    write_contract(admin_obj);
    write_deployment(kRv32AdminAddr, admin_obj);
    auto native_caller_obj = make_native_contract(kNativeCallerId);
    write_contract(native_caller_obj);
    write_deployment(kNativeCallerAddr, native_caller_obj);
    auto rv32_caller_obj = make_native_contract(kRv32CallerId);
    write_contract(rv32_caller_obj);
    write_deployment(kRv32CallerAddr, rv32_caller_obj);

    // Fund the native caller (native HOG balance) and, via the admin tx, the
    // RV32 caller (itc token balance).
    setup_grant_storage_token(kNativeCallerAddr, start_balance);

    // Pre-fund the recipient when caller-requested (e.g. CreditOverflow test).
    // Both backends bypass the per-backend funding limit: balances near
    // INT128_MAX are unreachable via normal supply flows but we need them to
    // exercise the credit-overflow code path.
    if (recipient_start_balance > 0) {
      // Native: write directly to the HOG balance slot, bypassing the
      // supply-cap assert in setup_grant_storage_token.
      write_item(make_free_native_token_balance_addrkey(native_receiver),
                 std::make_shared<const StorageItemWrapper>(
                     static_cast<int128_t>(recipient_start_balance),
                     get_default_cost_model()));

      // RV32: write directly to the itc_erc20 NonnegativeInt balance key,
      // bypassing the mint path (which would overflow the total-supply key).
      StorageKey rv32_balance_skey(StorageKeyType::ContractData);
      rv32_balance_skey.contract_data_key() =
          itc_erc20_balance_key(rv32_receiver);
      write_item(AddressAndKey{.address = kRv32TokenAddr,
                               .key = std::move(rv32_balance_skey)},
                 std::make_shared<const StorageItemWrapper>(
                     static_cast<int128_t>(recipient_start_balance),
                     get_default_cost_model()));
    }

    run_tx(kRv32AdminAddr, 0, ctx);

    return {run_capture_tx(kNativeCallerAddr, native_result_key, ctx),
            run_capture_tx(kRv32CallerAddr, rv32_result_key, ctx)};
  }

  // Runs transfer_from(owner, owner, transfer_amount) -- a delegated
  // self-transfer -- against both backends. The owner starts with
  // `owner_balance` and grants the spender (the caller) an allowance of
  // `allowance`. Returns {native_record, rv32_record}.
  std::pair<CapturedRecord, CapturedRecord>
  run_transfer_from_self_parity(uint128_t owner_balance, uint128_t allowance,
                                uint128_t transfer_amount)
  {
    auto native_result_key = make_storage_key("native_result");
    auto rv32_result_key = make_storage_key("rv32_result");

    // An owner driver that grants `spender` an allowance on `token`.
    auto make_grant = [](ContractAddress token, ContractAddress spender,
                         uint128_t amt) -> NativeContract::function_t {
      return [token, spender,
              amt](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        ContractTraitClient<itc_erc20::ERC20> client(token);
        auto r = client.invoke_opt<u8"adjust_allowance">(
            ctx, spender, make_difference(static_cast<int128_t>(amt)));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "grant allowance failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      };
    };

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_owner_fns;
    native_owner_fns.emplace(0, make_grant(kNativeTokenAddr, kNativeSpenderAddr,
                                           allowance));
    auto native_owner =
        std::make_unique<NativeContract>(std::move(native_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_owner_fns;
    rv32_owner_fns.emplace(0, make_grant(kRv32TokenAddr, kRv32SpenderAddr,
                                         allowance));
    auto rv32_owner =
        std::make_unique<NativeContract>(std::move(rv32_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_spender_fns;
    native_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                      kNativeTokenAddr, native_result_key,
                                      kNativeOwnerAddr, kNativeOwnerAddr,
                                      make_amount(transfer_amount)));
    auto native_spender =
        std::make_unique<NativeContract>(std::move(native_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_spender_fns;
    rv32_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                    kRv32TokenAddr, rv32_result_key,
                                    kRv32OwnerAddr, kRv32OwnerAddr,
                                    make_amount(transfer_amount)));
    auto rv32_spender =
        std::make_unique<NativeContract>(std::move(rv32_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0,
        [owner_balance](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kRv32AdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kRv32OwnerAddr,
                                                  make_amount(owner_balance));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kNativeOwnerId, std::move(native_owner));
    native_map.emplace(kNativeSpenderId, std::move(native_spender));
    native_map.emplace(kRv32OwnerId, std::move(rv32_owner));
    native_map.emplace(kRv32SpenderId, std::move(rv32_spender));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kRv32AdminAddr, kRv32AdminId);
    deploy_native(kNativeOwnerAddr, kNativeOwnerId);
    deploy_native(kNativeSpenderAddr, kNativeSpenderId);
    deploy_native(kRv32OwnerAddr, kRv32OwnerId);
    deploy_native(kRv32SpenderAddr, kRv32SpenderId);

    // Owner balances: native HOG grant for the native owner; admin mint for the
    // RV32 owner.
    setup_grant_storage_token(kNativeOwnerAddr, owner_balance);
    run_tx(kRv32AdminAddr, 0, ctx);

    // Owners grant allowances.
    run_tx(kNativeOwnerAddr, 0, ctx);
    run_tx(kRv32OwnerAddr, 0, ctx);

    // Spenders run the delegated transfer and capture the outcome.
    return {run_capture_tx(kNativeSpenderAddr, native_result_key, ctx),
            run_capture_tx(kRv32SpenderAddr, rv32_result_key, ctx)};
  }

  // Runs transfer_from(owner, distinct_recipient, transfer_amount) against both
  // backends. The owner starts with `owner_balance` and grants the spender an
  // allowance of `allowance`. The recipient is a distinct address from the
  // owner. Returns {native_record, rv32_record}.
  std::pair<CapturedRecord, CapturedRecord>
  run_transfer_from_parity(uint128_t owner_balance, uint128_t allowance,
                           uint128_t transfer_amount)
  {
    auto native_result_key = make_storage_key("native_result");
    auto rv32_result_key = make_storage_key("rv32_result");

    // An owner driver that grants `spender` an allowance on `token`.
    auto make_grant = [](ContractAddress token, ContractAddress spender,
                         uint128_t amt) -> NativeContract::function_t {
      return [token, spender,
              amt](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        ContractTraitClient<itc_erc20::ERC20> client(token);
        auto r = client.invoke_opt<u8"adjust_allowance">(
            ctx, spender, make_difference(static_cast<int128_t>(amt)));
        if (!r.has_value() || !r.value().has_value() ||
            !r.value().value().has_value() ||
            !r.value().value().value().is_ok()) {
          ADD_FAILURE() << "grant allowance failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      };
    };

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_owner_fns;
    native_owner_fns.emplace(0, make_grant(kNativeTokenAddr, kNativeSpenderAddr,
                                           allowance));
    auto native_owner =
        std::make_unique<NativeContract>(std::move(native_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_owner_fns;
    rv32_owner_fns.emplace(0, make_grant(kRv32TokenAddr, kRv32SpenderAddr,
                                         allowance));
    auto rv32_owner =
        std::make_unique<NativeContract>(std::move(rv32_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_spender_fns;
    native_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                      kNativeTokenAddr, native_result_key,
                                      kNativeOwnerAddr, kNativeTfRecipientAddr,
                                      make_amount(transfer_amount)));
    auto native_spender =
        std::make_unique<NativeContract>(std::move(native_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_spender_fns;
    rv32_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                    kRv32TokenAddr, rv32_result_key,
                                    kRv32OwnerAddr, kRv32TfRecipientAddr,
                                    make_amount(transfer_amount)));
    auto rv32_spender =
        std::make_unique<NativeContract>(std::move(rv32_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0,
        [owner_balance](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kRv32AdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kRv32OwnerAddr,
                                                  make_amount(owner_balance));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kNativeOwnerId, std::move(native_owner));
    native_map.emplace(kNativeSpenderId, std::move(native_spender));
    native_map.emplace(kRv32OwnerId, std::move(rv32_owner));
    native_map.emplace(kRv32SpenderId, std::move(rv32_spender));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kRv32AdminAddr, kRv32AdminId);
    deploy_native(kNativeOwnerAddr, kNativeOwnerId);
    deploy_native(kNativeSpenderAddr, kNativeSpenderId);
    deploy_native(kRv32OwnerAddr, kRv32OwnerId);
    deploy_native(kRv32SpenderAddr, kRv32SpenderId);

    // Owner balances: native HOG grant for the native owner; admin mint for the
    // RV32 owner.
    setup_grant_storage_token(kNativeOwnerAddr, owner_balance);
    run_tx(kRv32AdminAddr, 0, ctx);

    // Owners grant allowances.
    run_tx(kNativeOwnerAddr, 0, ctx);
    run_tx(kRv32OwnerAddr, 0, ctx);

    // Spenders run the delegated transfer and capture the outcome.
    return {run_capture_tx(kNativeSpenderAddr, native_result_key, ctx),
            run_capture_tx(kRv32SpenderAddr, rv32_result_key, ctx)};
  }

  // Runs adjust_allowance(self, +amount) -- a caller granting itself an
  // allowance -- against both backends. Returns {native_record, rv32_record}.
  std::pair<CapturedRecord, CapturedRecord>
  run_adjust_allowance_self_parity(uint128_t amount)
  {
    auto native_result_key = make_storage_key("native_result");
    auto rv32_result_key = make_storage_key("rv32_result");

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_caller_fns;
    native_caller_fns.emplace(
        0, make_capture_driver<u8"adjust_allowance">(
               kNativeTokenAddr, native_result_key, kNativeCallerAddr,
               make_difference(static_cast<int128_t>(amount))));
    auto native_caller =
        std::make_unique<NativeContract>(std::move(native_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_caller_fns;
    rv32_caller_fns.emplace(
        0, make_capture_driver<u8"adjust_allowance">(
               kRv32TokenAddr, rv32_result_key, kRv32CallerAddr,
               make_difference(static_cast<int128_t>(amount))));
    auto rv32_caller =
        std::make_unique<NativeContract>(std::move(rv32_caller_fns));

    // The RV32 admin just initializes itc_erc20 (no balance needed).
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kRv32AdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kNativeCallerId, std::move(native_caller));
    native_map.emplace(kRv32CallerId, std::move(rv32_caller));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kRv32AdminAddr, kRv32AdminId);
    deploy_native(kNativeCallerAddr, kNativeCallerId);
    deploy_native(kRv32CallerAddr, kRv32CallerId);

    run_tx(kRv32AdminAddr, 0, ctx);

    return {run_capture_tx(kNativeCallerAddr, native_result_key, ctx),
            run_capture_tx(kRv32CallerAddr, rv32_result_key, ctx)};
  }

  // `count` distinct capture result keys named `prefix`0..`prefix`N-1.
  static std::vector<StorageKeyContractData>
  make_result_keys(std::string const &prefix, size_t count)
  {
    std::vector<StorageKeyContractData> keys;
    keys.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      keys.push_back(make_storage_key(prefix + std::to_string(i)));
    }
    return keys;
  }

  // Compute the contract-data storage key that itc_erc20 uses for its total
  // supply: the SDK's .hash() on the bare b"total_supply" byte string. A CXS
  // fixed-opaque array encodes as its raw bytes, so the preimage is exactly
  // the 12 ASCII bytes of "total_supply".
  static StorageKeyContractData itc_erc20_total_supply_key()
  {
    // "total_supply" in ASCII/UTF-8
    static constexpr std::array<uint8_t, 12> kTag = {0x74, 0x6f, 0x74, 0x61,
                                                     0x6c, 0x5f, 0x73, 0x75,
                                                     0x70, 0x70, 0x6c, 0x79};
    return StorageKeyContractData{
        sha256(std::span<const uint8_t>(kTag.data(), kTag.size()))};
  }

  // Hand-craft committed token state, bypassing the mint path: direct NNInt
  // writes to each backend's total-supply and balance slots. These states
  // (supply near INT128_MAX, or balances a mint could never have funded) are
  // unreachable via normal supply flows but are needed to exercise the
  // supply-edge code paths.
  void write_native_total_supply(int128_t value)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = kNativeTokenTotalSupplyKey;
    write_item(
        AddressAndKey{.address = kNativeTokenAddr, .key = std::move(key)},
        std::make_shared<const StorageItemWrapper>(value,
                                                   get_default_cost_model()));
  }

  void write_rv32_total_supply(int128_t value)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = itc_erc20_total_supply_key();
    write_item(AddressAndKey{.address = kRv32TokenAddr, .key = std::move(key)},
               std::make_shared<const StorageItemWrapper>(
                   value, get_default_cost_model()));
  }

  void write_native_balance(ContractAddress const &addr, int128_t value)
  {
    write_item(make_free_native_token_balance_addrkey(addr),
               std::make_shared<const StorageItemWrapper>(
                   value, get_default_cost_model()));
  }

  void write_rv32_balance(ContractAddress const &addr, int128_t value)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = itc_erc20_balance_key(addr);
    write_item(AddressAndKey{.address = kRv32TokenAddr, .key = std::move(key)},
               std::make_shared<const StorageItemWrapper>(
                   value, get_default_cost_model()));
  }

  // Runs mint/burn capture steps against both backends. The native token's
  // mint and burn are gated on msg_sender == kGenesisRootContractAddress, so
  // the capture driver plays the genesis root: the fixture's genesis is empty,
  // nothing occupies that address, and the test installs its own root
  // stand-in there (the "custom genesis" of these tests). The rv32 itc_erc20
  // is initialized with the genesis root as its owner, so the same caller
  // address passes both backends' owner gates.
  //
  // `native_steps` run from `native_caller` against the native token (driver
  // method 0); `rv32_steps` run from `rv32_caller` against the rv32 token
  // (method 1). Pass ordinary caller addresses to exercise the unauthorized
  // paths. `pre_capture_setup` runs after deployment and rv32 initialization,
  // just before the capture transactions -- the hook for hand-crafting
  // storage state (supply keys, balances).
  std::pair<MultiStepOutcome, MultiStepOutcome> run_mint_burn_parity(
      std::vector<CaptureStep> native_steps,
      std::vector<CaptureStep> rv32_steps,
      ContractAddress native_caller = kGenesisRootContractAddress,
      ContractAddress rv32_caller = kGenesisRootContractAddress,
      std::function<void()> const &pre_capture_setup = {})
  {
    std::vector<StorageKeyContractData> native_keys;
    native_keys.reserve(native_steps.size());
    for (auto const &step : native_steps) {
      native_keys.push_back(step.result_key);
    }
    std::vector<StorageKeyContractData> rv32_keys;
    rv32_keys.reserve(rv32_steps.size());
    for (auto const &step : rv32_steps) {
      rv32_keys.push_back(step.result_key);
    }

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        driver_fns;
    driver_fns.emplace(0, make_multi_capture_driver(std::move(native_steps)));
    driver_fns.emplace(1, make_multi_capture_driver(std::move(rv32_steps)));
    auto driver = std::make_unique<NativeContract>(std::move(driver_fns));

    // The RV32 admin initializes itc_erc20 with the genesis root as owner.
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        rv32_admin_fns;
    rv32_admin_fns.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kRv32TokenAddr);
          auto init =
              client.invoke_opt<u8"initialize">(ctx,
                                                kGenesisRootContractAddress);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto rv32_admin =
        std::make_unique<NativeContract>(std::move(rv32_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kRv32AdminId, std::move(rv32_admin));
    native_map.emplace(kMintBurnDriverId, std::move(driver));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto rv32_token_obj = erc20_meta::load_rv32();
    EXPECT_TRUE(rv32_token_obj);
    write_contract(rv32_token_obj);
    write_deployment(kRv32TokenAddr, rv32_token_obj);

    auto admin_obj = make_native_contract(kRv32AdminId);
    write_contract(admin_obj);
    write_deployment(kRv32AdminAddr, admin_obj);

    auto driver_obj = make_native_contract(kMintBurnDriverId);
    write_contract(driver_obj);
    write_deployment(native_caller, driver_obj);
    if (rv32_caller != native_caller) {
      write_deployment(rv32_caller, driver_obj);
    }

    run_tx(kRv32AdminAddr, 0, ctx);
    if (pre_capture_setup) {
      pre_capture_setup();
    }

    return {run_multi_capture_tx(native_caller, native_keys, ctx,
                                 /*method_id=*/0),
            run_multi_capture_tx(rv32_caller, rv32_keys, ctx,
                                 /*method_id=*/1)};
  }
};

// Baseline: an ordinary funded transfer to a distinct recipient. Both backends
// must accept it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedTransferToRecipientParity)
{
  auto [native_record, rv32_record] = run_transfer_parity(
      /*start_balance=*/1'000, /*transfer_amount=*/37, kNativeRecipientAddr,
      kRv32RecipientAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on funded transfer";
}

// A funded self-transfer: the caller owns more than it sends to itself. Both
// backends must accept it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedSelfTransferParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/1'000, /*transfer_amount=*/37,
                          kNativeCallerAddr, kRv32CallerAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on funded "
         "self-transfer";
}

// Over-balance self-transfer: both backends now reject at invoke with
// ERC20Error::TooLarge, byte-identically (closes the #2134 behavior; the
// native token's strict per-debit check matches itc_erc20).
TEST_F(Erc20ParityTest, OverBalanceSelfTransferParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/10, /*transfer_amount=*/100,
                          kNativeCallerAddr, kRv32CallerAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on over-balance "
         "self-transfer";
  ASSERT_GE(rv32_record.size(), 3U);
  EXPECT_EQ(rv32_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort))
      << "both backends must reject at invoke";
}

// A funded delegated self-transfer: a spender with sufficient allowance moves
// tokens from owner to owner, with the owner funded. Both backends must accept
// it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedTransferFromSelfParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/1'000, /*allowance=*/100, /*transfer_amount=*/37);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on funded "
         "transfer_from self";
}

// Over-balance delegated self-transfer: both backends reject at invoke with
// ERC20Error::TooLarge (byte-identical). That the staged allowance debit
// reverts with the aborted frame is verified in
// NativeTokenAllowanceTest.OverBalanceTransferFromRevertsAllowanceDebit.
TEST_F(Erc20ParityTest, OverBalanceTransferFromSelfParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/10, /*allowance=*/100, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on over-balance "
         "transfer_from";
  ASSERT_GE(rv32_record.size(), 3U);
  EXPECT_EQ(rv32_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// Boundary: transfer_from of the owner's exact full balance (allowance
// ample) succeeds on both backends.
TEST_F(Erc20ParityTest, ExactBalanceTransferFromParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/100, /*allowance=*/200, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, rv32_record);
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
}

// Boundary: one past the owner's balance (allowance ample) aborts on both
// backends with identical ERC20Error::TooLarge bytes.
TEST_F(Erc20ParityTest, OneOverBalanceTransferFromParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/100, /*allowance=*/200, /*transfer_amount=*/101);
  EXPECT_EQ(native_record, rv32_record);
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// Both allowance and balance insufficient: identical outcome on both
// backends. (Both legs throw the same TooLarge bytes, so this can't
// distinguish which leg fired -- the structural ordering below matches the
// reference so no future error-code split can diverge.)
TEST_F(Erc20ParityTest, BothInsufficientTransferFromParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/10, /*allowance=*/10, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, rv32_record);
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// An over-balance transfer to a distinct recipient: both backends reject at
// invoke with ERC20Error::TooLarge, the aborted child stages nothing, and
// the surrounding transaction commits cleanly on both.
TEST_F(Erc20ParityTest, OverBalanceTransferToRecipientParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/10, /*transfer_amount=*/100,
                          kNativeRecipientAddr, kRv32RecipientAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on over-balance "
         "transfer to a distinct recipient";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(static_cast<ExecResult>(native_record[0]), ExecResult::ExecResultOK)
      << "the transaction must commit (nothing staged by the aborted child)";
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// Boundary: a transfer of the caller's exact full balance lands the balance
// at exactly zero and SUCCEEDS on both backends (the reference errors only
// when the resulting total would be < 0).
TEST_F(Erc20ParityTest, ExactBalanceTransferParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/100, /*transfer_amount=*/100,
                          kNativeRecipientAddr, kRv32RecipientAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on exact-balance "
         "transfer";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "exact-balance transfer must succeed";
}

// Boundary: one token over the full balance aborts on both backends with
// identical ERC20Error::TooLarge bytes.
TEST_F(Erc20ParityTest, OneOverBalanceTransferParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/100, /*transfer_amount=*/101,
                          kNativeRecipientAddr, kRv32RecipientAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged one past the "
         "balance boundary";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// Credit-side overflow beyond the supply cap: the receiver already holds
// INT128_MAX, which cannot occur under the total-supply bound (~2^59) but
// can be hand-crafted in tests. Native no longer checks the credit leg at
// invoke (supply-bound makes it unreachable); the RV32 reference still
// checks it, so the two backends diverge on this artificial state.
// Native accepts the transfer at invoke; the transaction is then rejected
// at commit by HogDb's add-capacity bound. RV32 rejects at invoke with
// ERC20Error::TooLarge. Documented divergence, not a bug.
TEST_F(Erc20ParityTest, CreditOverflowBeyondSupplyDiverges)
{
  auto [native_record, rv32_record] = run_transfer_parity(
      /*start_balance=*/100, /*transfer_amount=*/1, kNativeRecipientAddr,
      kRv32RecipientAddr,
      /*recipient_start_balance=*/
      uint128_t{std::numeric_limits<int128_t>::max()});
  // Native: invoke accepted, tx rejected at commit (add-capacity bound).
  ASSERT_EQ(native_record.size(), 1U);
  EXPECT_NE(static_cast<ExecResult>(native_record[0]),
            ExecResult::ExecResultOK);
  // RV32: invoke-time Abort, tx commits (nothing staged by the aborted child).
  ASSERT_GE(rv32_record.size(), 3U);
  EXPECT_EQ(rv32_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_NE(native_record, rv32_record);
}

// An amount above INT128_MAX (not representable as a signed balance delta):
// pin that both backends reject it with the same observable result.
TEST_F(Erc20ParityTest, HugeAmountTransferParity)
{
  auto [native_record, rv32_record] = run_transfer_parity(
      /*start_balance=*/1'000, /*transfer_amount=*/uint128_t{1} << 127,
      kNativeRecipientAddr, kRv32RecipientAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on amount > "
         "INT128_MAX";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// amount > INT128_MAX → both backends abort with TooLarge at the conversion,
// before the allowance debit.
TEST_F(Erc20ParityTest, HugeAmountTransferFromParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/1'000, /*allowance=*/1'000,
      /*transfer_amount=*/uint128_t{1} << 127);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on transfer_from "
         "amount > INT128_MAX";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// transfer_from with sufficient balance but insufficient allowance. Both
// allowance and balance debits are strict (invoke-time TooLarge rejection);
// this test pins the allowance leg's byte-identical rejection between the
// native precompile and the RV32 itc_erc20 reference.
TEST_F(Erc20ParityTest, InsufficientAllowanceTransferFromParity)
{
  auto [native_record, rv32_record] = run_transfer_from_self_parity(
      /*owner_balance=*/1'000, /*allowance=*/10, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on insufficient-"
         "allowance transfer_from";
}

// A funded transfer to the all-zero address. The native HOG precompile treats
// the zero address as an ordinary recipient (issue #2135, characterized as
// working-as-intended), and itc_erc20 has no zero-recipient guard either, so
// both should accept it identically. This locks in that agreement.
TEST_F(Erc20ParityTest, TransferToZeroAddressParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/1'000, /*transfer_amount=*/37,
                          ContractAddress{}, ContractAddress{});
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on transfer to the "
         "zero address";
}

// A zero-amount funded transfer. A no-op move that both backends should accept
// identically (including any emitted Transfer event).
TEST_F(Erc20ParityTest, ZeroAmountTransferParity)
{
  auto [native_record, rv32_record] =
      run_transfer_parity(/*start_balance=*/1'000, /*transfer_amount=*/0,
                          kNativeRecipientAddr, kRv32RecipientAddr);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on zero-amount "
         "transfer";
}

// A funded transfer_from to a distinct recipient: a spender with sufficient
// allowance moves tokens from owner to a third-party recipient. Both backends
// must accept it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedTransferFromToRecipientParity)
{
  auto [native_record, rv32_record] = run_transfer_from_parity(
      /*owner_balance=*/1'000, /*allowance=*/200, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on funded "
         "transfer_from to a distinct recipient";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
}

// One past the owner balance in a transfer_from to a distinct recipient: both
// backends reject at invoke with ERC20Error::TooLarge, byte-identically.
TEST_F(Erc20ParityTest, OneOverBalanceTransferFromToRecipientParity)
{
  auto [native_record, rv32_record] = run_transfer_from_parity(
      /*owner_balance=*/100, /*allowance=*/200, /*transfer_amount=*/101);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged one past the "
         "balance boundary on transfer_from to a distinct recipient";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// adjust_allowance(self, +amount): granting oneself an allowance is rejected by
// both backends with ERC20Error::Internal at invoke, byte-identically.
TEST_F(Erc20ParityTest, AdjustAllowanceToSelfParity)
{
  auto [native_record, rv32_record] =
      run_adjust_allowance_self_parity(/*amount=*/100);
  EXPECT_EQ(native_record, rv32_record)
      << "native precompile and RV32 itc_erc20 diverged on adjust_allowance "
         "to self";
}

// hogd#1918, allowance-netting form: decrementing a fresh (owner, spender)
// allowance and immediately incrementing it back, in one transaction, must not
// launder the invalid decrement. Both backends reject the decrement at invoke
// (ERC20Error::TooLarge) and let the increment through, so the committed
// allowance is exactly the increment -- byte-identically. Before the strict
// NNInt update, the decrement returned Ok (and emitted an Approval event for a
// diff nothing could fund), and the increment netted the pending add back to
// zero so the root commit found nothing to reject; that history would show up
// here as a Return status on step 0 and a zero trailing allowance read. The
// event-side counterpart lives in
// NativeRv32TokenEventParityTest.NettedNegativeApprovalEmitsNoEventRv32AndNative.
TEST_F(Erc20ParityTest, NegativeAllowanceNettingParity)
{
  auto [native_outcome, rv32_outcome] =
      run_allowance_netting_parity(/*magnitude=*/100);
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a netted negative "
         "allowance adjustment";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK)
      << "the transaction must commit (the rejected decrement stages nothing)";
  ASSERT_EQ(records.size(), 4U);

  // Step 0: the decrement below a fresh (zero) allowance is rejected at
  // invoke, not deferred to commit and netted away. The records compare equal
  // byte-for-byte above, so pinning the error on one backend pins both.
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort))
      << "a decrement below a fresh allowance must abort at invoke";
  EXPECT_EQ(step_erc20_error<u8"adjust_allowance">(records[0]),
            std::optional(itc_erc20::ERC20Error::TooLarge))
      << "the rejected decrement must abort with a decodable TooLarge";

  // Step 1: the sibling increment is unaffected by the aborted decrement.
  ASSERT_GE(records[1].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "the compensating increment must still succeed on its own merits";

  // Steps 2 and 3: the committed (caller, spender) allowance is the increment
  // alone, so it must differ from the never-touched control allowance. Under
  // the netting bug both adjustments landed and cancelled, and this read
  // returned the same Ok(0) bytes as the control.
  ASSERT_GE(records[2].size(), 2U);
  EXPECT_EQ(records[2][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(records[3][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_NE(records[2], records[3])
      << "the committed allowance must be the increment, not a netted zero";
}

// hogd#1918, cross-child balance-netting form: a caller with no tokens
// transfers `amount` to an owner and then, as a sibling call in the same
// transaction, pulls the same `amount` back via an allowance the owner granted
// it. The compensating credit must not rescue the unfunded debit: both
// backends reject the first transfer at invoke (ERC20Error::TooLarge),
// byte-identically, and the delegated transfer that follows still succeeds, so
// the tokens end up with the caller and the owner is left at zero.
TEST_F(Erc20ParityTest, UnfundedTransferNettedByLaterCreditParity)
{
  auto [native_outcome, rv32_outcome] =
      run_unfunded_transfer_netting_parity(/*amount=*/100);
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an unfunded "
         "transfer netted by a later credit";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK)
      << "the transaction must commit (the rejected transfer stages nothing)";
  ASSERT_EQ(records.size(), 5U);

  // Step 0: the unfunded transfer aborts at invoke. Under the netting bug it
  // returned Ok and published a Transfer event the caller could not fund. The
  // records compare equal byte-for-byte above, so pinning the error on one
  // backend pins both.
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort))
      << "a transfer from a zero balance must abort at invoke";
  EXPECT_EQ(step_erc20_error<u8"transfer">(records[0]),
            std::optional(itc_erc20::ERC20Error::TooLarge))
      << "the unfunded transfer must abort with a decodable TooLarge";

  // Step 1: the delegated pull is a legitimate, fully-funded transfer.
  ASSERT_GE(records[1].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "the allowance-backed transfer_from must still succeed";

  // Steps 2-4: the committed balances reflect the delegated pull only. The
  // caller holds the tokens (so its read differs from the never-touched
  // control) and the owner holds nothing (so its read matches the control).
  // Under the netting bug the two debits cancelled and the roles were
  // reversed: the caller read back zero and the owner kept the tokens.
  ASSERT_GE(records[2].size(), 2U);
  ASSERT_GE(records[3].size(), 2U);
  ASSERT_GE(records[4].size(), 2U);
  EXPECT_EQ(records[2][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(records[3][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(records[4][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_NE(records[2], records[4])
      << "the caller must end up holding the tokens the owner started with";
  EXPECT_EQ(records[3], records[4])
      << "the owner must be left with nothing after the delegated pull";
}

// ---------------------------------------------------------------------------
// Mint/burn parity. The native token's mint and burn are only callable by the
// genesis root contract, so these tests deploy the capture driver AT the
// genesis root address (empty test genesis; see run_mint_burn_parity) and
// initialize the rv32 itc_erc20 with the genesis root as its owner, so the
// same caller passes both backends' owner gates.
// ---------------------------------------------------------------------------

// Baseline: an authorized mint to a fresh recipient. Both backends accept it
// byte-identically, and the committed supply and balance equal the minted
// amount.
TEST_F(Erc20ParityTest, FundedMintParity)
{
  auto nk = make_result_keys("native_step", 3);
  auto rk = make_result_keys("rv32_step", 3);
  auto steps = [](ContractAddress token, ContractAddress recipient,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], recipient, make_amount(1'000)),
        make_step<u8"total_supply">(token, keys[1]),
        make_step<u8"get_balance_of">(token, keys[2], recipient)};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an authorized mint";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 3U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "an authorized mint must succeed";
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[1]),
            std::optional<uint128_t>(1'000));
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[2]),
            std::optional<uint128_t>(1'000));
}

// A zero-amount mint: a no-op credit both backends accept identically
// (including the zero-amount Transfer event path).
TEST_F(Erc20ParityTest, ZeroAmountMintParity)
{
  auto nk = make_result_keys("native_step", 3);
  auto rk = make_result_keys("rv32_step", 3);
  auto steps = [](ContractAddress token, ContractAddress recipient,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], recipient, make_amount(0)),
        make_step<u8"total_supply">(token, keys[1]),
        make_step<u8"get_balance_of">(token, keys[2], recipient)};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a zero-amount mint";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 3U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[1]),
            std::optional<uint128_t>(0));
}

// An amount above INT128_MAX (not representable as a signed balance delta):
// both backends reject the mint with the same TooLarge bytes, staging nothing.
TEST_F(Erc20ParityTest, HugeAmountMintParity)
{
  auto nk = make_result_keys("native_step", 2);
  auto rk = make_result_keys("rv32_step", 2);
  auto steps = [](ContractAddress token, ContractAddress recipient,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], recipient,
                            make_amount(uint128_t{1} << 127)),
        make_step<u8"total_supply">(token, keys[1])};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on mint amount > "
         "INT128_MAX";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 2U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"mint">(records[0]),
            std::optional(itc_erc20::ERC20Error::TooLarge));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[1]),
            std::optional<uint128_t>(0));
}

// Credit-side overflow: the recipient already holds INT128_MAX (hand-crafted;
// unreachable under the supply bound). The credit leg fails on both backends
// with the same TooLarge bytes -- native's supply pre-read stages nothing, and
// the reference's strict balance inc fires before its supply add.
TEST_F(Erc20ParityTest, MintToMaxBalanceRecipientParity)
{
  auto nk = make_result_keys("native_step", 2);
  auto rk = make_result_keys("rv32_step", 2);
  auto steps = [](ContractAddress token, ContractAddress recipient,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], recipient, make_amount(1)),
        make_step<u8"total_supply">(token, keys[1])};
  };
  auto [native_outcome, rv32_outcome] = run_mint_burn_parity(
      steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
      steps(kRv32TokenAddr, kRv32RecipientAddr, rk),
      kGenesisRootContractAddress, kGenesisRootContractAddress, [this] {
        write_native_balance(kNativeRecipientAddr,
                             std::numeric_limits<int128_t>::max());
        write_rv32_balance(kRv32RecipientAddr,
                           std::numeric_limits<int128_t>::max());
      });
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a mint credit "
         "overflowing the recipient balance";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 2U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"mint">(records[0]),
            std::optional(itc_erc20::ERC20Error::TooLarge));
}

// A mint from an ordinary (non-genesis-root) caller: both backends reject it
// with the reference's ERC20Error::MustBeContractOwner, byte-identically.
TEST_F(Erc20ParityTest, UnauthorizedMintParity)
{
  auto nk = make_result_keys("native_step", 2);
  auto rk = make_result_keys("rv32_step", 2);
  auto steps = [](ContractAddress token, ContractAddress recipient,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], recipient, make_amount(100)),
        make_step<u8"total_supply">(token, keys[1])};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk),
                           kNativeCallerAddr, kRv32CallerAddr);
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an unauthorized "
         "mint";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 2U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"mint">(records[0]),
            std::optional(itc_erc20::ERC20Error::MustBeContractOwner));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[1]),
            std::optional<uint128_t>(0));
}

// Baseline burn: mint then burn part of it. Both backends accept both ops
// byte-identically, and supply and balance reflect the difference.
TEST_F(Erc20ParityTest, FundedBurnParity)
{
  auto nk = make_result_keys("native_step", 4);
  auto rk = make_result_keys("rv32_step", 4);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], holder, make_amount(1'000)),
        make_step<u8"burn">(token, keys[1], holder, make_amount(400)),
        make_step<u8"total_supply">(token, keys[2]),
        make_step<u8"get_balance_of">(token, keys[3], holder)};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an authorized burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 4U);
  ASSERT_GE(records[1].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "an authorized funded burn must succeed";
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[2]),
            std::optional<uint128_t>(600));
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[3]),
            std::optional<uint128_t>(600));
}

// Boundary: burning the holder's exact full balance lands balance and supply
// at zero and succeeds on both backends.
TEST_F(Erc20ParityTest, ExactBalanceBurnParity)
{
  auto nk = make_result_keys("native_step", 4);
  auto rk = make_result_keys("rv32_step", 4);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], holder, make_amount(1'000)),
        make_step<u8"burn">(token, keys[1], holder, make_amount(1'000)),
        make_step<u8"total_supply">(token, keys[2]),
        make_step<u8"get_balance_of">(token, keys[3], holder)};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an exact-balance "
         "burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 4U);
  ASSERT_GE(records[1].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "an exact-balance burn must succeed";
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[2]),
            std::optional<uint128_t>(0));
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[3]),
            std::optional<uint128_t>(0));
}

// Boundary: one past the holder's balance aborts on both backends with
// identical TooLarge bytes, leaving balance and supply untouched.
TEST_F(Erc20ParityTest, OneOverBalanceBurnParity)
{
  auto nk = make_result_keys("native_step", 4);
  auto rk = make_result_keys("rv32_step", 4);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], holder, make_amount(1'000)),
        make_step<u8"burn">(token, keys[1], holder, make_amount(1'001)),
        make_step<u8"total_supply">(token, keys[2]),
        make_step<u8"get_balance_of">(token, keys[3], holder)};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged one past the balance "
         "boundary on burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 4U);
  ASSERT_GE(records[1].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"burn">(records[1]),
            std::optional(itc_erc20::ERC20Error::TooLarge));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[2]),
            std::optional<uint128_t>(1'000));
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[3]),
            std::optional<uint128_t>(1'000));
}

// A burn from a never-funded holder (no prior mint at all, so the native
// supply key does not even exist): the strict balance debit fires first on
// both backends, with identical TooLarge bytes.
TEST_F(Erc20ParityTest, UnfundedBurnParity)
{
  auto nk = make_result_keys("native_step", 2);
  auto rk = make_result_keys("rv32_step", 2);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{make_step<u8"burn">(token, keys[0], holder,
                                                        make_amount(50)),
                                    make_step<u8"total_supply">(token,
                                                                keys[1])};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an unfunded burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 2U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"burn">(records[0]),
            std::optional(itc_erc20::ERC20Error::TooLarge));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[1]),
            std::optional<uint128_t>(0));
}

// A burn amount above INT128_MAX: both backends reject at the amount
// conversion with identical TooLarge bytes, before touching any state.
TEST_F(Erc20ParityTest, HugeAmountBurnParity)
{
  auto nk = make_result_keys("native_step", 2);
  auto rk = make_result_keys("rv32_step", 2);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], holder, make_amount(10)),
        make_step<u8"burn">(token, keys[1], holder,
                            make_amount(uint128_t{1} << 127))};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on burn amount > "
         "INT128_MAX";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 2U);
  ASSERT_GE(records[1].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"burn">(records[1]),
            std::optional(itc_erc20::ERC20Error::TooLarge));
}

// A burn from an ordinary (non-genesis-root) caller: both backends reject it
// with the reference's ERC20Error::MustBeContractOwner, byte-identically.
TEST_F(Erc20ParityTest, UnauthorizedBurnParity)
{
  auto nk = make_result_keys("native_step", 1);
  auto rk = make_result_keys("rv32_step", 1);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"burn">(token, keys[0], holder, make_amount(100))};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk),
                           kNativeCallerAddr, kRv32CallerAddr);
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an unauthorized "
         "burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 1U);
  ASSERT_GE(records[0].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_EQ(step_erc20_error<u8"burn">(records[0]),
            std::optional(itc_erc20::ERC20Error::MustBeContractOwner));
}

// Supply accumulation across several operations in one transaction: an
// initial empty-state read, two mints to distinct recipients, a partial burn,
// then reads pinning supply = N + M - K and the individual balances --
// byte-identically on both backends.
TEST_F(Erc20ParityTest, TotalSupplyTracksMintBurnParity)
{
  auto nk = make_result_keys("native_step", 7);
  auto rk = make_result_keys("rv32_step", 7);
  auto steps = [](ContractAddress token, ContractAddress holder_a,
                  ContractAddress holder_b,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"total_supply">(token, keys[0]),
        make_step<u8"mint">(token, keys[1], holder_a, make_amount(1'000)),
        make_step<u8"mint">(token, keys[2], holder_b, make_amount(500)),
        make_step<u8"burn">(token, keys[3], holder_a, make_amount(300)),
        make_step<u8"total_supply">(token, keys[4]),
        make_step<u8"get_balance_of">(token, keys[5], holder_a),
        make_step<u8"get_balance_of">(token, keys[6], holder_b)};
  };
  auto [native_outcome, rv32_outcome] = run_mint_burn_parity(
      steps(kNativeTokenAddr, kNativeRecipientAddr, kNativeTfRecipientAddr, nk),
      steps(kRv32TokenAddr, kRv32RecipientAddr, kRv32TfRecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on supply tracking "
         "across mint/mint/burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 7U);
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[0]),
            std::optional<uint128_t>(0));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[4]),
            std::optional<uint128_t>(1'200));
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[5]),
            std::optional<uint128_t>(700));
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[6]),
            std::optional<uint128_t>(500));
}

// Mint and an equal burn as siblings in one transaction: the burn sees the
// running in-tx supply and balance, both succeed, and the committed supply
// nets back to the pre-mint value -- byte-identically on both backends.
TEST_F(Erc20ParityTest, MintThenBurnSameTxNetsSupplyParity)
{
  auto nk = make_result_keys("native_step", 5);
  auto rk = make_result_keys("rv32_step", 5);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"total_supply">(token, keys[0]),
        make_step<u8"mint">(token, keys[1], holder, make_amount(1'000)),
        make_step<u8"burn">(token, keys[2], holder, make_amount(1'000)),
        make_step<u8"total_supply">(token, keys[3]),
        make_step<u8"get_balance_of">(token, keys[4], holder)};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a same-tx "
         "mint-then-burn";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK);
  ASSERT_EQ(records.size(), 5U);
  ASSERT_GE(records[1].size(), 2U);
  ASSERT_GE(records[2].size(), 2U);
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(records[2][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return))
      << "burning tokens minted earlier in the same tx must succeed";
  EXPECT_EQ(records[0], records[3])
      << "the committed supply must net back to the pre-mint value";
  EXPECT_EQ(step_ok_amount<u8"get_balance_of">(records[4]),
            std::optional<uint128_t>(0));
}

// A burn against hand-crafted state where the balance exists but the supply
// key was never written: the strict balance debit passes at invoke, the
// supply decrement is deferred, and the commit rejects the transaction on
// both backends (the supply floor would go negative) -- byte-identically.
TEST_F(Erc20ParityTest, BurnWithoutSupplyKeyParity)
{
  auto nk = make_result_keys("native_step", 1);
  auto rk = make_result_keys("rv32_step", 1);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"burn">(token, keys[0], holder, make_amount(50))};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk),
                           kGenesisRootContractAddress,
                           kGenesisRootContractAddress, [this] {
                             write_native_balance(kNativeRecipientAddr, 100);
                             write_rv32_balance(kRv32RecipientAddr, 100);
                           });
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a burn with no "
         "supply key";
  EXPECT_NE(rv32_outcome.first, ExecResult::ExecResultOK)
      << "the deferred supply decrement must be rejected at commit";
}

// A mint whose supply increment overflows the committed total supply
// (hand-crafted near INT128_MAX): the increment is deferred like the
// reference's non-strict supply add, the child returns Ok, and the commit
// rejects the transaction on both backends -- byte-identically.
TEST_F(Erc20ParityTest, MintSupplyOverflowParity)
{
  auto nk = make_result_keys("native_step", 1);
  auto rk = make_result_keys("rv32_step", 1);
  auto steps = [](ContractAddress token, ContractAddress recipient,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], recipient, make_amount(100))};
  };
  auto [native_outcome, rv32_outcome] = run_mint_burn_parity(
      steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
      steps(kRv32TokenAddr, kRv32RecipientAddr, rk),
      kGenesisRootContractAddress, kGenesisRootContractAddress, [this] {
        write_native_total_supply(std::numeric_limits<int128_t>::max() - 10);
        write_rv32_total_supply(std::numeric_limits<int128_t>::max() - 10);
      });
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a mint "
         "overflowing the committed total supply";
  EXPECT_NE(rv32_outcome.first, ExecResult::ExecResultOK)
      << "the deferred supply increment must be rejected at commit";
}

// A burn of more than the committed total supply against hand-crafted
// inconsistent state (balance > supply, unreachable organically since supply
// bounds every balance): the strict balance debit passes at invoke, the
// deferred supply decrement leaves a negative final total, and the commit
// rejects the transaction on both backends -- byte-identically.
TEST_F(Erc20ParityTest, BurnBeyondSupplyParity)
{
  auto nk = make_result_keys("native_step", 1);
  auto rk = make_result_keys("rv32_step", 1);
  auto steps = [](ContractAddress token, ContractAddress holder,
                  std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"burn">(token, keys[0], holder, make_amount(50))};
  };
  auto [native_outcome, rv32_outcome] =
      run_mint_burn_parity(steps(kNativeTokenAddr, kNativeRecipientAddr, nk),
                           steps(kRv32TokenAddr, kRv32RecipientAddr, rk),
                           kGenesisRootContractAddress,
                           kGenesisRootContractAddress, [this] {
                             write_native_balance(kNativeRecipientAddr, 100);
                             write_native_total_supply(10);
                             write_rv32_balance(kRv32RecipientAddr, 100);
                             write_rv32_total_supply(10);
                           });
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on a burn beyond the "
         "committed total supply";
  EXPECT_NE(rv32_outcome.first, ExecResult::ExecResultOK)
      << "the deferred supply decrement must be rejected at commit";
}

// Overflow of the in-transaction supply add accumulator: a mint of
// INT128_MAX followed by a sibling mint of 1 in the same transaction. The
// second mint's supply increment overflows the accumulated in-tx add, which
// the reference reports by throwing LibraryExn::NonNegativeIntOverflow (a
// library abort, not an ERC20 error). A compensating burn nets the staged
// supply and balance back to zero so the transaction commits and the step
// records survive for comparison. Byte-identical on both backends.
TEST_F(Erc20ParityTest, MintSupplyAccumulatorOverflowParity)
{
  constexpr uint128_t kMax = std::numeric_limits<int128_t>::max();
  auto nk = make_result_keys("native_step", 4);
  auto rk = make_result_keys("rv32_step", 4);
  auto steps = [&](ContractAddress token, ContractAddress holder_a,
                   ContractAddress holder_b,
                   std::vector<StorageKeyContractData> const &keys) {
    return std::vector<CaptureStep>{
        make_step<u8"mint">(token, keys[0], holder_a, make_amount(kMax)),
        make_step<u8"mint">(token, keys[1], holder_b, make_amount(1)),
        make_step<u8"burn">(token, keys[2], holder_a, make_amount(kMax)),
        make_step<u8"total_supply">(token, keys[3])};
  };
  auto [native_outcome, rv32_outcome] = run_mint_burn_parity(
      steps(kNativeTokenAddr, kNativeRecipientAddr, kNativeTfRecipientAddr, nk),
      steps(kRv32TokenAddr, kRv32RecipientAddr, kRv32TfRecipientAddr, rk));
  EXPECT_EQ(native_outcome, rv32_outcome)
      << "native precompile and RV32 itc_erc20 diverged on an in-tx supply "
         "accumulator overflow";

  auto const &[commit, records] = rv32_outcome;
  EXPECT_EQ(commit, ExecResult::ExecResultOK)
      << "the netted transaction must commit";
  ASSERT_EQ(records.size(), 4U);
  ASSERT_GE(records[0].size(), 2U);
  ASSERT_GE(records[1].size(), 2U);
  ASSERT_GE(records[2].size(), 2U);
  EXPECT_EQ(records[0][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(records[1][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort))
      << "the accumulator-overflowing mint must abort at invoke";
  EXPECT_EQ(step_erc20_error<u8"mint">(records[1]), std::nullopt)
      << "the abort must be a library exception, not an ERC20 error";
  EXPECT_EQ(records[2][1],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
  EXPECT_EQ(step_ok_amount<u8"total_supply">(records[3]),
            std::optional<uint128_t>(0));
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
