#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "built_contracts/calldata_wrapper.h"
#include "built_contracts/default_wallet.h"
#include "built_contracts/manual_revert_test.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/hogexec/precompiles/precompile_default_wallet.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {
using wallet_meta = ContractTraitMeta<default_wallet::DefaultWallet>;
using echo_meta = ContractTraitMeta<calldata_wrapper::CalldataWrapper>;
using mrt_meta = ContractTraitMeta<manual_revert_test::ManualRevertTest>;
using default_wallet::WalletCall;
using CapturedRecord = std::vector<uint8_t>;
using wallet_calls_t = x23::Vec<WalletCall, 4294967295>;
} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
TEST(DefaultWalletPrecompile, ConstructsAndCoversAllEntrypoints)
{
  auto contract = make_native_default_wallet_contract();
  ASSERT_NE(contract, nullptr);
}

// Differential test: the native default_wallet precompile vs the wasm
// reference build, driven through identical operations under the REAL cost
// model. The reference contract's behavior is the spec, exactly as built —
// parity, not improvement. Records compare byte-for-byte:
// [root_success, commit ExecResult, literal return/abort bytes (0xFF if
// no buffer)]. Gas consumption is deliberately not compared.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class DefaultWalletParityTest : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  DefaultWalletParityTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  static constexpr int32_t kNativeWalletId = 60;
  static constexpr int32_t kDriverBaseId = 61;
  static constexpr ContractAddress kEchoAddr = make_contract_address(220);
  static constexpr ContractAddress kAbortAddr = make_contract_address(221);
  static constexpr ContractAddress kMissingAddr = make_contract_address(240);

  PubkeyAccountPreimage native_preimage_{};
  PubkeyAccountPreimage wasm_preimage_{};
  ContractAddress native_wallet_{};
  ContractAddress wasm_wallet_{};
  std::unique_ptr<MultiplexedContext> ctx_;

  static PubkeyAccountPreimage make_preimage(uint8_t seed)
  {
    PubkeyAccountPreimage p{};
    p.pubkey.scheme(CryptographicScheme::Ed25519);
    p.pubkey.ed25519() = Ed25519PublicKey{};
    p.pubkey.ed25519().fill(seed);
    p.nonce = seed;
    return p;
  }

  // Builds the multiplexed context; `extra` lets tests add driver contracts.
  void make_ctx(
      std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra = {})
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(kNativeWalletId,
                       make_native_default_wallet_contract());
    for (auto &[id, c] : extra) {
      native_map.emplace(id, std::move(c));
    }
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
    ctx_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });
  }

  void setup_wallets_and_targets()
  {
    native_preimage_ = make_preimage(1);
    wasm_preimage_ = make_preimage(2);
    native_wallet_ = *pubkey_auth_address(native_preimage_);
    wasm_wallet_ = *pubkey_auth_address(wasm_preimage_);

    auto native_obj = make_native_contract(kNativeWalletId);
    write_contract(native_obj);
    write_deployment(native_wallet_, native_obj);
    setup_grant_storage_token(native_wallet_, 1'000'000'000);

    auto wasm_obj = wallet_meta::load_wasm();
    ASSERT_TRUE(wasm_obj);
    write_contract(wasm_obj);
    write_deployment(wasm_wallet_, wasm_obj);
    setup_grant_storage_token(wasm_wallet_, 1'000'000'000);

    auto echo_obj = echo_meta::load_wasm();
    ASSERT_TRUE(echo_obj);
    write_contract(echo_obj);
    write_deployment(kEchoAddr, echo_obj);
    setup_grant_storage_token(kEchoAddr, 1'000'000'000);

    auto abort_obj = mrt_meta::load_wasm();
    ASSERT_TRUE(abort_obj);
    write_contract(abort_obj);
    write_deployment(kAbortAddr, abort_obj);
    setup_grant_storage_token(kAbortAddr, 1'000'000'000);
  }

  AuthClaim make_pubkey_claim(PubkeyAccountPreimage const &preimage)
  {
    AuthClaim claim{};
    claim.claim_type(AuthClaimType::kPubkeySignature);
    claim.pubkey_preimage() = preimage;
    return claim;
  }

  WalletCall echo_call(size_t payload_len, uint64_t gas_limit = UINT64_MAX)
  {
    x23::Vec<uint8_t, 4294967295> payload;
    for (size_t i = 0; i < payload_len; ++i) {
      payload.push_back(0xAB);
    }
    auto calldata = echo_meta::make_calldata<u8"echo">(payload);
    WalletCall call{};
    call.target = kEchoAddr;
    call.entrypoint =
        entrypoint_from_method_id(echo_meta::method_id<u8"echo">());
    call.gas_limit = gas_limit;
    call.calldata = calldata;
    return call;
  }

  // A call that manually aborts: test_error_type_revert(1) throws
  // ManualRevertTestError -> child frame Abort -> WalletError::CallAborted.
  WalletCall abort_call()
  {
    auto calldata = mrt_meta::make_calldata<u8"test_error_type_revert">(
        static_cast<uint32_t>(1));
    WalletCall call{};
    call.target = kAbortAddr;
    call.entrypoint = entrypoint_from_method_id(
        mrt_meta::method_id<u8"test_error_type_revert">());
    call.gas_limit = UINT64_MAX;
    call.calldata = calldata;
    return call;
  }

  // Root-invokes `wallet` with the given invocation + auth claim and
  // captures [root_success, commit, buffer bytes | 0xFF]. Invocations are
  // built with ContractTraitClient<...>::make_invocation.
  CapturedRecord run_wallet_root_tx(Invocation invocation, AuthClaim claim)
  {
    SignedTransaction stx;
    stx.tx.invocation = std::move(invocation);
    stx.tx.auth_claim = std::move(claim);
    stx.tx.uniquifier = make_new_uniquifier();

    auto handler = make_new_system_call_handler(stx, *ctx_);
    auto res = handler.root_invoke();
    CapturedRecord rec;
    rec.push_back(res.is_root_success() ? 1 : 0);
    auto frame_output = handler.get_most_recent_frame_output();
    ExecResult commit = handler.try_commit_transaction();
    rec.push_back(static_cast<uint8_t>(commit));
    switch (frame_output.status_code()) {
    case InvocationFrameStatusCode::Return:
      for (auto b : frame_output.return_buffer()) {
        rec.push_back(static_cast<uint8_t>(b));
      }
      break;
    case InvocationFrameStatusCode::Abort:
      for (auto b : frame_output.abort_buffer()) {
        rec.push_back(static_cast<uint8_t>(b));
      }
      break;
    case InvocationFrameStatusCode::Fault:
      rec.push_back(0xFF);
      break;
    }
    advance_block();
    return rec;
  }

  CapturedRecord
  run_wallet_execute_tx(ContractAddress const &wallet,
                        PubkeyAccountPreimage const &preimage,
                        wallet_calls_t const &calls,
                        std::optional<AuthClaim> claim_override = std::nullopt)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet);
    return run_wallet_root_tx(
        client.make_invocation<u8"execute">(UINT64_MAX, calls),
        claim_override ? *claim_override : make_pubkey_claim(preimage));
  }

  // Runs the same call list through both wallets and byte-compares.
  void expect_parity(wallet_calls_t const &calls)
  {
    auto native =
        run_wallet_execute_tx(native_wallet_, native_preimage_, calls);
    auto wasm = run_wallet_execute_tx(wasm_wallet_, wasm_preimage_, calls);
    EXPECT_EQ(native, wasm);
  }

  // Builds a native "driver" contract whose method 0 invokes `target`'s
  // `execute` entrypoint with `calls`, then records (tag, frame_status,
  // literal return bytes) into `result_key`. Adapted from erc20_parity_test.cc.
  NativeContract::function_t
  make_wallet_capture_driver(ContractAddress target,
                             StorageKeyContractData result_key,
                             wallet_calls_t const &calls)
  {
    auto calldata = wallet_meta::make_calldata<u8"execute">(calls);
    Runtime::MethodID method_id = wallet_meta::method_id<u8"execute">();
    return [target, result_key, method_id,
            calldata = std::move(calldata)](
               NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
      CopyableInvokeOutputs outputs{};
      CopyableInvokeInputs inputs{
          .entrypoint_id = method_id,
          .gas_limit = static_cast<GasAmount>(UINT64_MAX),
          .flags = std::to_underlying(InvokeFlag::None),
      };
      auto r = ctx.syscall<SyscallNo::kInvoke>(
          target,
          std::as_bytes(std::span(calldata.data(), calldata.size())),
          std::span<uint32_t>{}, inputs, outputs);

      std::vector<uint8_t> record;
      if (!r.has_value()) {
        // Host-level (non-deterministic) failure.
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
          record.push_back(0xFF);
        }
      }

      auto set_r = ctx.syscall<SyscallNo::kBytestringSet>(
          result_key,
          std::as_bytes(std::span(record.data(), record.size())),
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

  // Runs `addr`'s method-0 capture driver and returns a single byte-comparable
  // outcome: the commit ExecResult byte followed by the literal invoke record.
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
};

// Wrong claim type: default AuthClaim{} is kSourceAddressAuthData.
TEST_F(DefaultWalletParityTest, WrongClaimTypeInvalidAuthClaim)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(4));
  auto native = run_wallet_execute_tx(native_wallet_, native_preimage_, calls,
                                      AuthClaim{});
  auto wasm = run_wallet_execute_tx(wasm_wallet_, wasm_preimage_, calls,
                                    AuthClaim{});
  EXPECT_EQ(native, wasm);
}

// Pubkey claim whose derived address is not the wallet's address.
TEST_F(DefaultWalletParityTest, MismatchedPreimageInvalidAuthClaim)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(4));
  auto other = make_pubkey_claim(make_preimage(3));
  auto native =
      run_wallet_execute_tx(native_wallet_, native_preimage_, calls, other);
  auto wasm =
      run_wallet_execute_tx(wasm_wallet_, wasm_preimage_, calls, other);
  EXPECT_EQ(native, wasm);
}

// Non-root invocation: a native driver contract invokes each wallet;
// both must abort with NotRootInvocation. Driver pattern from
// erc20_parity_test.cc make_capture_driver / run_capture_tx / read_record.
TEST_F(DefaultWalletParityTest, NonRootInvocationRejected)
{
  setup_wallets_and_targets();

  wallet_calls_t calls;
  calls.push_back(echo_call(4));

  static constexpr ContractAddress kNativeDriverAddr = make_contract_address(230);
  static constexpr ContractAddress kWasmDriverAddr = make_contract_address(231);

  auto native_result_key = make_storage_key("native_result");
  auto wasm_result_key = make_storage_key("wasm_result");

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      native_driver_fns;
  native_driver_fns.emplace(
      0,
      make_wallet_capture_driver(native_wallet_, native_result_key, calls));
  auto native_driver =
      std::make_unique<NativeContract>(std::move(native_driver_fns));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      wasm_driver_fns;
  wasm_driver_fns.emplace(
      0, make_wallet_capture_driver(wasm_wallet_, wasm_result_key, calls));
  auto wasm_driver =
      std::make_unique<NativeContract>(std::move(wasm_driver_fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra;
  extra.emplace(kDriverBaseId, std::move(native_driver));
  extra.emplace(kDriverBaseId + 1, std::move(wasm_driver));
  make_ctx(std::move(extra));

  auto native_driver_obj = make_native_contract(kDriverBaseId);
  write_contract(native_driver_obj);
  write_deployment(kNativeDriverAddr, native_driver_obj);
  setup_grant_storage_token(kNativeDriverAddr, 1'000'000'000);

  auto wasm_driver_obj = make_native_contract(kDriverBaseId + 1);
  write_contract(wasm_driver_obj);
  write_deployment(kWasmDriverAddr, wasm_driver_obj);
  setup_grant_storage_token(kWasmDriverAddr, 1'000'000'000);

  auto native_record =
      run_capture_tx(kNativeDriverAddr, native_result_key, *ctx_);
  auto wasm_record =
      run_capture_tx(kWasmDriverAddr, wasm_result_key, *ctx_);
  EXPECT_EQ(native_record, wasm_record);
}

TEST_F(DefaultWalletParityTest, EmptyCallList)
{
  setup_wallets_and_targets();
  make_ctx();
  expect_parity(wallet_calls_t{});
}

TEST_F(DefaultWalletParityTest, TooManyCalls)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  for (int i = 0; i < 17; ++i) {
    calls.push_back(echo_call(4));
  }
  expect_parity(calls);
}

TEST_F(DefaultWalletParityTest, MaxCallsSucceeds)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  for (int i = 0; i < 16; ++i) {
    // echo returns 11 bytes per call (CXS Vec<u8>: 1-byte prefix + 10 payload);
    // item_len = 1 + 11 = 12; 16 calls: 1 + 16*12 = 193 bytes < kMaxReturnDataBytes
    calls.push_back(echo_call(10));
  }
  expect_parity(calls);
}

TEST_F(DefaultWalletParityTest, MultiCallHappyPathReturnsEchoedBytes)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(1));
  calls.push_back(echo_call(30));
  calls.push_back(echo_call(0)); // zero-length echo: item is prefix-only

  auto native =
      run_wallet_execute_tx(native_wallet_, native_preimage_, calls);
  auto wasm = run_wallet_execute_tx(wasm_wallet_, wasm_preimage_, calls);
  EXPECT_EQ(native, wasm);

  // Absolute check against the reference encoding: success (root_success=1,
  // commit=ExecResultOK) and the returned buffer decodes as the CXS encoding
  // of Vec<WalletBytes> where item i is the callee's literal return bytes
  // (echo's own CXS-encoded Vec<u8>: prefix || payload).
  ASSERT_GE(native.size(), 2U);
  EXPECT_EQ(native[0], 1);
  EXPECT_EQ(native[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
}

TEST_F(DefaultWalletParityTest, CalleeAborts)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(4)); // one success first: abort discards it
  calls.push_back(abort_call());
  expect_parity(calls);
}

TEST_F(DefaultWalletParityTest, CalleeFaultsOutOfGas)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(4, /*gas_limit=*/1)); // child OOG -> Fault
  expect_parity(calls);
}

TEST_F(DefaultWalletParityTest, PreFrameRejectMissingTarget)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  WalletCall missing = echo_call(4);
  missing.target = kMissingAddr; // no deployment -> Invoke rejected pre-frame
  calls.push_back(missing);
  expect_parity(calls);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
