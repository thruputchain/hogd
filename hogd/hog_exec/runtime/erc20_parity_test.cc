#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "built_contracts/itc_erc20.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
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
// the WASM itc_erc20 charge storage differently (native balance_cost vs plain
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

} // namespace

// Drives the native HOG precompile and the WASM itc_erc20 contract through the
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
      GenesisConfigurationLoader::kPresetNativeTokenAddress;
  static constexpr ContractAddress kWasmTokenAddr = make_contract_address(200);
  static constexpr ContractAddress kWasmAdminAddr = make_contract_address(201);
  static constexpr ContractAddress kNativeCallerAddr =
      make_contract_address(202);
  static constexpr ContractAddress kWasmCallerAddr = make_contract_address(203);
  // Distinct (non-self) recipients for the baseline transfer.
  static constexpr ContractAddress kNativeRecipientAddr =
      make_contract_address(204);
  static constexpr ContractAddress kWasmRecipientAddr =
      make_contract_address(205);
  // transfer_from: an owner (token holder, allowance granter) and a spender
  // (the delegated caller), per backend.
  static constexpr ContractAddress kNativeOwnerAddr =
      make_contract_address(206);
  static constexpr ContractAddress kNativeSpenderAddr =
      make_contract_address(207);
  static constexpr ContractAddress kWasmOwnerAddr = make_contract_address(208);
  static constexpr ContractAddress kWasmSpenderAddr =
      make_contract_address(209);
  // transfer_from to a distinct recipient: two extra recipient addresses.
  static constexpr ContractAddress kNativeTfRecipientAddr =
      make_contract_address(210);
  static constexpr ContractAddress kWasmTfRecipientAddr =
      make_contract_address(211);

  // Native contract ids in the multiplexed context's native map.
  static constexpr int32_t kWasmAdminId = 50;
  static constexpr int32_t kNativeCallerId = 51;
  static constexpr int32_t kWasmCallerId = 52;
  static constexpr int32_t kNativeOwnerId = 53;
  static constexpr int32_t kNativeSpenderId = 54;
  static constexpr int32_t kWasmOwnerId = 55;
  static constexpr int32_t kWasmSpenderId = 56;

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
  // balance. The WASM SDK's .hash() CXS-encodes the tuple (b"balance", addr)
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
      CopyableInvokeOutputs outputs{};
      CopyableInvokeInputs inputs{
          .entrypoint_id = method_id,
          .gas_limit = static_cast<GasAmount>(UINT64_MAX),
          .flags = std::to_underlying(InvokeFlag::None),
      };
      auto r = ctx.syscall<SyscallNo::kInvoke>(
          target, std::as_bytes(std::span(calldata.data(), calldata.size())),
          std::span<uint32_t>{}, inputs, outputs);

      std::vector<uint8_t> record;
      if (!r.has_value()) {
        // Host-level (non-deterministic) failure: should not happen on these
        // paths, but record it distinctly rather than hiding it.
        record.push_back(0xEE);
        record.push_back(static_cast<uint8_t>(r.error().code()));
      }
      else if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        // Invoke rejected before any child frame ran.
        record.push_back(0x01);
        record.push_back(static_cast<uint8_t>(r.value()));
      }
      else {
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

      auto set_r = ctx.syscall<SyscallNo::kBytestringSet>(
          result_key, std::as_bytes(std::span(record.data(), record.size())),
          std::to_underlying(StorageDeltaFlags::None));
      if (!set_r.has_value() ||
          set_r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        ADD_FAILURE() << "capture driver failed to persist result";
        return InvokeError{InvokeErrorCode::Deterministic};
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

  // Runs `transfer(receiver, transfer_amount)` against both the native
  // precompile and the WASM itc_erc20, with each caller starting from a
  // freshly-minted balance of `start_balance`. Pass each caller's own address
  // as the receiver for a self-transfer, or a distinct address for an ordinary
  // transfer. When recipient_start_balance > 0 the receiver is pre-funded on
  // both backends before the transfer runs (native: direct write bypassing the
  // supply-cap guard; WASM: direct write to the itc_erc20 balance key).
  // Returns {native_record, wasm_record}, the byte-for-byte captured outcomes
  // (tag + frame status + literal return/abort bytes).
  std::pair<CapturedRecord, CapturedRecord>
  run_transfer_parity(uint128_t start_balance, uint128_t transfer_amount,
                      ContractAddress native_receiver,
                      ContractAddress wasm_receiver,
                      uint128_t recipient_start_balance = 0)
  {
    auto native_result_key = make_storage_key("native_result");
    auto wasm_result_key = make_storage_key("wasm_result");

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_caller_fns;
    native_caller_fns.emplace(
        0, make_capture_driver<u8"transfer">(kNativeTokenAddr,
                                             native_result_key, native_receiver,
                                             make_amount(transfer_amount)));
    auto native_caller =
        std::make_unique<NativeContract>(std::move(native_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_caller_fns;
    wasm_caller_fns.emplace(
        0, make_capture_driver<u8"transfer">(kWasmTokenAddr, wasm_result_key,
                                             wasm_receiver,
                                             make_amount(transfer_amount)));
    auto wasm_caller =
        std::make_unique<NativeContract>(std::move(wasm_caller_fns));

    // The WASM admin initializes itc_erc20 (as its own-address owner) and mints
    // the caller's starting balance.
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_admin_fns;
    wasm_admin_fns.emplace(
        0,
        [start_balance](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kWasmTokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kWasmAdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kWasmCallerAddr,
                                                  make_amount(start_balance));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto wasm_admin =
        std::make_unique<NativeContract>(std::move(wasm_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kWasmAdminId, std::move(wasm_admin));
    native_map.emplace(kNativeCallerId, std::move(native_caller));
    native_map.emplace(kWasmCallerId, std::move(wasm_caller));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto wasm_token_obj = erc20_meta::load_wasm();
    EXPECT_TRUE(wasm_token_obj);
    write_contract(wasm_token_obj);
    write_deployment(kWasmTokenAddr, wasm_token_obj);

    auto admin_obj = make_native_contract(kWasmAdminId);
    write_contract(admin_obj);
    write_deployment(kWasmAdminAddr, admin_obj);
    auto native_caller_obj = make_native_contract(kNativeCallerId);
    write_contract(native_caller_obj);
    write_deployment(kNativeCallerAddr, native_caller_obj);
    auto wasm_caller_obj = make_native_contract(kWasmCallerId);
    write_contract(wasm_caller_obj);
    write_deployment(kWasmCallerAddr, wasm_caller_obj);

    // Fund the native caller (native HOG balance) and, via the admin tx, the
    // WASM caller (itc token balance).
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

      // WASM: write directly to the itc_erc20 NonnegativeInt balance key,
      // bypassing the mint path (which would overflow the total-supply key).
      StorageKey wasm_balance_skey(StorageKeyType::ContractData);
      wasm_balance_skey.contract_data_key() =
          itc_erc20_balance_key(wasm_receiver);
      write_item(AddressAndKey{.address = kWasmTokenAddr,
                               .key = std::move(wasm_balance_skey)},
                 std::make_shared<const StorageItemWrapper>(
                     static_cast<int128_t>(recipient_start_balance),
                     get_default_cost_model()));
    }

    run_tx(kWasmAdminAddr, 0, ctx);

    return {run_capture_tx(kNativeCallerAddr, native_result_key, ctx),
            run_capture_tx(kWasmCallerAddr, wasm_result_key, ctx)};
  }

  // Runs transfer_from(owner, owner, transfer_amount) -- a delegated
  // self-transfer -- against both backends. The owner starts with
  // `owner_balance` and grants the spender (the caller) an allowance of
  // `allowance`. Returns {native_record, wasm_record}.
  std::pair<CapturedRecord, CapturedRecord>
  run_transfer_from_self_parity(uint128_t owner_balance, uint128_t allowance,
                                uint128_t transfer_amount)
  {
    auto native_result_key = make_storage_key("native_result");
    auto wasm_result_key = make_storage_key("wasm_result");

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
        wasm_owner_fns;
    wasm_owner_fns.emplace(0, make_grant(kWasmTokenAddr, kWasmSpenderAddr,
                                         allowance));
    auto wasm_owner =
        std::make_unique<NativeContract>(std::move(wasm_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_spender_fns;
    native_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                      kNativeTokenAddr, native_result_key,
                                      kNativeOwnerAddr, kNativeOwnerAddr,
                                      make_amount(transfer_amount)));
    auto native_spender =
        std::make_unique<NativeContract>(std::move(native_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_spender_fns;
    wasm_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                    kWasmTokenAddr, wasm_result_key,
                                    kWasmOwnerAddr, kWasmOwnerAddr,
                                    make_amount(transfer_amount)));
    auto wasm_spender =
        std::make_unique<NativeContract>(std::move(wasm_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_admin_fns;
    wasm_admin_fns.emplace(
        0,
        [owner_balance](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kWasmTokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kWasmAdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kWasmOwnerAddr,
                                                  make_amount(owner_balance));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto wasm_admin =
        std::make_unique<NativeContract>(std::move(wasm_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kWasmAdminId, std::move(wasm_admin));
    native_map.emplace(kNativeOwnerId, std::move(native_owner));
    native_map.emplace(kNativeSpenderId, std::move(native_spender));
    native_map.emplace(kWasmOwnerId, std::move(wasm_owner));
    native_map.emplace(kWasmSpenderId, std::move(wasm_spender));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto wasm_token_obj = erc20_meta::load_wasm();
    EXPECT_TRUE(wasm_token_obj);
    write_contract(wasm_token_obj);
    write_deployment(kWasmTokenAddr, wasm_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kWasmAdminAddr, kWasmAdminId);
    deploy_native(kNativeOwnerAddr, kNativeOwnerId);
    deploy_native(kNativeSpenderAddr, kNativeSpenderId);
    deploy_native(kWasmOwnerAddr, kWasmOwnerId);
    deploy_native(kWasmSpenderAddr, kWasmSpenderId);

    // Owner balances: native HOG grant for the native owner; admin mint for the
    // WASM owner.
    setup_grant_storage_token(kNativeOwnerAddr, owner_balance);
    run_tx(kWasmAdminAddr, 0, ctx);

    // Owners grant allowances.
    run_tx(kNativeOwnerAddr, 0, ctx);
    run_tx(kWasmOwnerAddr, 0, ctx);

    // Spenders run the delegated transfer and capture the outcome.
    return {run_capture_tx(kNativeSpenderAddr, native_result_key, ctx),
            run_capture_tx(kWasmSpenderAddr, wasm_result_key, ctx)};
  }

  // Runs transfer_from(owner, distinct_recipient, transfer_amount) against both
  // backends. The owner starts with `owner_balance` and grants the spender an
  // allowance of `allowance`. The recipient is a distinct address from the
  // owner. Returns {native_record, wasm_record}.
  std::pair<CapturedRecord, CapturedRecord>
  run_transfer_from_parity(uint128_t owner_balance, uint128_t allowance,
                           uint128_t transfer_amount)
  {
    auto native_result_key = make_storage_key("native_result");
    auto wasm_result_key = make_storage_key("wasm_result");

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
        wasm_owner_fns;
    wasm_owner_fns.emplace(0, make_grant(kWasmTokenAddr, kWasmSpenderAddr,
                                         allowance));
    auto wasm_owner =
        std::make_unique<NativeContract>(std::move(wasm_owner_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_spender_fns;
    native_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                      kNativeTokenAddr, native_result_key,
                                      kNativeOwnerAddr, kNativeTfRecipientAddr,
                                      make_amount(transfer_amount)));
    auto native_spender =
        std::make_unique<NativeContract>(std::move(native_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_spender_fns;
    wasm_spender_fns.emplace(0, make_capture_driver<u8"transfer_from">(
                                    kWasmTokenAddr, wasm_result_key,
                                    kWasmOwnerAddr, kWasmTfRecipientAddr,
                                    make_amount(transfer_amount)));
    auto wasm_spender =
        std::make_unique<NativeContract>(std::move(wasm_spender_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_admin_fns;
    wasm_admin_fns.emplace(
        0,
        [owner_balance](
            NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kWasmTokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kWasmAdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto mint = client.invoke_opt<u8"mint">(ctx, kWasmOwnerAddr,
                                                  make_amount(owner_balance));
          if (!mint.has_value() || !mint.value().has_value() ||
              !mint.value().value().has_value() ||
              !mint.value().value().value().is_ok()) {
            ADD_FAILURE() << "itc mint failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto wasm_admin =
        std::make_unique<NativeContract>(std::move(wasm_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kWasmAdminId, std::move(wasm_admin));
    native_map.emplace(kNativeOwnerId, std::move(native_owner));
    native_map.emplace(kNativeSpenderId, std::move(native_spender));
    native_map.emplace(kWasmOwnerId, std::move(wasm_owner));
    native_map.emplace(kWasmSpenderId, std::move(wasm_spender));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto wasm_token_obj = erc20_meta::load_wasm();
    EXPECT_TRUE(wasm_token_obj);
    write_contract(wasm_token_obj);
    write_deployment(kWasmTokenAddr, wasm_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kWasmAdminAddr, kWasmAdminId);
    deploy_native(kNativeOwnerAddr, kNativeOwnerId);
    deploy_native(kNativeSpenderAddr, kNativeSpenderId);
    deploy_native(kWasmOwnerAddr, kWasmOwnerId);
    deploy_native(kWasmSpenderAddr, kWasmSpenderId);

    // Owner balances: native HOG grant for the native owner; admin mint for the
    // WASM owner.
    setup_grant_storage_token(kNativeOwnerAddr, owner_balance);
    run_tx(kWasmAdminAddr, 0, ctx);

    // Owners grant allowances.
    run_tx(kNativeOwnerAddr, 0, ctx);
    run_tx(kWasmOwnerAddr, 0, ctx);

    // Spenders run the delegated transfer and capture the outcome.
    return {run_capture_tx(kNativeSpenderAddr, native_result_key, ctx),
            run_capture_tx(kWasmSpenderAddr, wasm_result_key, ctx)};
  }

  // Runs adjust_allowance(self, +amount) -- a caller granting itself an
  // allowance -- against both backends. Returns {native_record, wasm_record}.
  std::pair<CapturedRecord, CapturedRecord>
  run_adjust_allowance_self_parity(uint128_t amount)
  {
    auto native_result_key = make_storage_key("native_result");
    auto wasm_result_key = make_storage_key("wasm_result");

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        native_caller_fns;
    native_caller_fns.emplace(
        0, make_capture_driver<u8"adjust_allowance">(
               kNativeTokenAddr, native_result_key, kNativeCallerAddr,
               make_difference(static_cast<int128_t>(amount))));
    auto native_caller =
        std::make_unique<NativeContract>(std::move(native_caller_fns));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_caller_fns;
    wasm_caller_fns.emplace(
        0, make_capture_driver<u8"adjust_allowance">(
               kWasmTokenAddr, wasm_result_key, kWasmCallerAddr,
               make_difference(static_cast<int128_t>(amount))));
    auto wasm_caller =
        std::make_unique<NativeContract>(std::move(wasm_caller_fns));

    // The WASM admin just initializes itc_erc20 (no balance needed).
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        wasm_admin_fns;
    wasm_admin_fns.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> client(kWasmTokenAddr);
          auto init = client.invoke_opt<u8"initialize">(ctx, kWasmAdminAddr);
          if (!init.has_value() || !init.value().has_value()) {
            ADD_FAILURE() << "itc initialize failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });
    auto wasm_admin =
        std::make_unique<NativeContract>(std::move(wasm_admin_fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    native_map.emplace(kWasmAdminId, std::move(wasm_admin));
    native_map.emplace(kNativeCallerId, std::move(native_caller));
    native_map.emplace(kWasmCallerId, std::move(wasm_caller));
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    MultiplexedContext ctx(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });

    auto native_token_obj = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(native_token_obj);
    write_deployment(kNativeTokenAddr, native_token_obj);

    auto wasm_token_obj = erc20_meta::load_wasm();
    EXPECT_TRUE(wasm_token_obj);
    write_contract(wasm_token_obj);
    write_deployment(kWasmTokenAddr, wasm_token_obj);

    auto deploy_native = [this](ContractAddress addr, int32_t id) {
      auto obj = make_native_contract(id);
      write_contract(obj);
      write_deployment(addr, obj);
    };
    deploy_native(kWasmAdminAddr, kWasmAdminId);
    deploy_native(kNativeCallerAddr, kNativeCallerId);
    deploy_native(kWasmCallerAddr, kWasmCallerId);

    run_tx(kWasmAdminAddr, 0, ctx);

    return {run_capture_tx(kNativeCallerAddr, native_result_key, ctx),
            run_capture_tx(kWasmCallerAddr, wasm_result_key, ctx)};
  }
};

// Baseline: an ordinary funded transfer to a distinct recipient. Both backends
// must accept it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedTransferToRecipientParity)
{
  auto [native_record, wasm_record] = run_transfer_parity(
      /*start_balance=*/1'000, /*transfer_amount=*/37, kNativeRecipientAddr,
      kWasmRecipientAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on funded transfer";
}

// A funded self-transfer: the caller owns more than it sends to itself. Both
// backends must accept it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedSelfTransferParity)
{
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/1'000, /*transfer_amount=*/37,
                          kNativeCallerAddr, kWasmCallerAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on funded "
         "self-transfer";
}

// Over-balance self-transfer: both backends now reject at invoke with
// ERC20Error::TooLarge, byte-identically (closes the #2134 behavior; the
// native token's strict per-debit check matches itc_erc20).
TEST_F(Erc20ParityTest, OverBalanceSelfTransferParity)
{
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/10, /*transfer_amount=*/100,
                          kNativeCallerAddr, kWasmCallerAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on over-balance "
         "self-transfer";
  ASSERT_GE(wasm_record.size(), 3U);
  EXPECT_EQ(wasm_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort))
      << "both backends must reject at invoke";
}

// A funded delegated self-transfer: a spender with sufficient allowance moves
// tokens from owner to owner, with the owner funded. Both backends must accept
// it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedTransferFromSelfParity)
{
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/1'000, /*allowance=*/100, /*transfer_amount=*/37);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on funded "
         "transfer_from self";
}

// Over-balance delegated self-transfer: both backends reject at invoke with
// ERC20Error::TooLarge (byte-identical). That the staged allowance debit
// reverts with the aborted frame is verified in
// NativeTokenAllowanceTest.OverBalanceTransferFromRevertsAllowanceDebit.
TEST_F(Erc20ParityTest, OverBalanceTransferFromSelfParity)
{
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/10, /*allowance=*/100, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on over-balance "
         "transfer_from";
  ASSERT_GE(wasm_record.size(), 3U);
  EXPECT_EQ(wasm_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// Boundary: transfer_from of the owner's exact full balance (allowance
// ample) succeeds on both backends.
TEST_F(Erc20ParityTest, ExactBalanceTransferFromParity)
{
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/100, /*allowance=*/200, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, wasm_record);
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
}

// Boundary: one past the owner's balance (allowance ample) aborts on both
// backends with identical ERC20Error::TooLarge bytes.
TEST_F(Erc20ParityTest, OneOverBalanceTransferFromParity)
{
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/100, /*allowance=*/200, /*transfer_amount=*/101);
  EXPECT_EQ(native_record, wasm_record);
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
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/10, /*allowance=*/10, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, wasm_record);
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// An over-balance transfer to a distinct recipient: both backends reject at
// invoke with ERC20Error::TooLarge, the aborted child stages nothing, and
// the surrounding transaction commits cleanly on both.
TEST_F(Erc20ParityTest, OverBalanceTransferToRecipientParity)
{
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/10, /*transfer_amount=*/100,
                          kNativeRecipientAddr, kWasmRecipientAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on over-balance "
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
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/100, /*transfer_amount=*/100,
                          kNativeRecipientAddr, kWasmRecipientAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on exact-balance "
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
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/100, /*transfer_amount=*/101,
                          kNativeRecipientAddr, kWasmRecipientAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged one past the "
         "balance boundary";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// Credit-side overflow beyond the supply cap: the receiver already holds
// INT128_MAX, which cannot occur under the total-supply bound (~2^59) but
// can be hand-crafted in tests. Native no longer checks the credit leg at
// invoke (supply-bound makes it unreachable); the WASM reference still
// checks it, so the two backends diverge on this artificial state.
// Native accepts the transfer at invoke; the transaction is then rejected
// at commit by HogDb's add-capacity bound. WASM rejects at invoke with
// ERC20Error::TooLarge. Documented divergence, not a bug.
TEST_F(Erc20ParityTest, CreditOverflowBeyondSupplyDiverges)
{
  auto [native_record, wasm_record] = run_transfer_parity(
      /*start_balance=*/100, /*transfer_amount=*/1, kNativeRecipientAddr,
      kWasmRecipientAddr,
      /*recipient_start_balance=*/
      uint128_t{std::numeric_limits<int128_t>::max()});
  // Native: invoke accepted, tx rejected at commit (add-capacity bound).
  ASSERT_EQ(native_record.size(), 1U);
  EXPECT_NE(static_cast<ExecResult>(native_record[0]),
            ExecResult::ExecResultOK);
  // WASM: invoke-time Abort, tx commits (nothing staged by the aborted child).
  ASSERT_GE(wasm_record.size(), 3U);
  EXPECT_EQ(wasm_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_NE(native_record, wasm_record);
}

// An amount above INT128_MAX (not representable as a signed balance delta):
// pin that both backends reject it with the same observable result.
TEST_F(Erc20ParityTest, HugeAmountTransferParity)
{
  auto [native_record, wasm_record] = run_transfer_parity(
      /*start_balance=*/1'000, /*transfer_amount=*/uint128_t{1} << 127,
      kNativeRecipientAddr, kWasmRecipientAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on amount > "
         "INT128_MAX";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// amount > INT128_MAX → both backends abort with TooLarge at the conversion,
// before the allowance debit.
TEST_F(Erc20ParityTest, HugeAmountTransferFromParity)
{
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/1'000, /*allowance=*/1'000,
      /*transfer_amount=*/uint128_t{1} << 127);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on transfer_from "
         "amount > INT128_MAX";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// transfer_from with sufficient balance but insufficient allowance. Both
// allowance and balance debits are strict (invoke-time TooLarge rejection);
// this test pins the allowance leg's byte-identical rejection between the
// native precompile and the WASM itc_erc20 reference.
TEST_F(Erc20ParityTest, InsufficientAllowanceTransferFromParity)
{
  auto [native_record, wasm_record] = run_transfer_from_self_parity(
      /*owner_balance=*/1'000, /*allowance=*/10, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on insufficient-"
         "allowance transfer_from";
}

// A funded transfer to the all-zero address. The native HOG precompile treats
// the zero address as an ordinary recipient (issue #2135, characterized as
// working-as-intended), and itc_erc20 has no zero-recipient guard either, so
// both should accept it identically. This locks in that agreement.
TEST_F(Erc20ParityTest, TransferToZeroAddressParity)
{
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/1'000, /*transfer_amount=*/37,
                          ContractAddress{}, ContractAddress{});
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on transfer to the "
         "zero address";
}

// A zero-amount funded transfer. A no-op move that both backends should accept
// identically (including any emitted Transfer event).
TEST_F(Erc20ParityTest, ZeroAmountTransferParity)
{
  auto [native_record, wasm_record] =
      run_transfer_parity(/*start_balance=*/1'000, /*transfer_amount=*/0,
                          kNativeRecipientAddr, kWasmRecipientAddr);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on zero-amount "
         "transfer";
}

// A funded transfer_from to a distinct recipient: a spender with sufficient
// allowance moves tokens from owner to a third-party recipient. Both backends
// must accept it and return byte-identical results.
TEST_F(Erc20ParityTest, FundedTransferFromToRecipientParity)
{
  auto [native_record, wasm_record] = run_transfer_from_parity(
      /*owner_balance=*/1'000, /*allowance=*/200, /*transfer_amount=*/100);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on funded "
         "transfer_from to a distinct recipient";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Return));
}

// One past the owner balance in a transfer_from to a distinct recipient: both
// backends reject at invoke with ERC20Error::TooLarge, byte-identically.
TEST_F(Erc20ParityTest, OneOverBalanceTransferFromToRecipientParity)
{
  auto [native_record, wasm_record] = run_transfer_from_parity(
      /*owner_balance=*/100, /*allowance=*/200, /*transfer_amount=*/101);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged one past the "
         "balance boundary on transfer_from to a distinct recipient";
  ASSERT_GE(native_record.size(), 3U);
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
}

// adjust_allowance(self, +amount): granting oneself an allowance is rejected by
// both backends with ERC20Error::Internal at invoke, byte-identically.
TEST_F(Erc20ParityTest, AdjustAllowanceToSelfParity)
{
  auto [native_record, wasm_record] =
      run_adjust_allowance_self_parity(/*amount=*/100);
  EXPECT_EQ(native_record, wasm_record)
      << "native precompile and WASM itc_erc20 diverged on adjust_allowance "
         "to self";
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
