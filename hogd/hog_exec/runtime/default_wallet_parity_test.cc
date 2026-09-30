#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "auth_token.hh"
#include "built_contracts/calldata_wrapper.h"
#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
#include "built_contracts/manual_revert_test.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"
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
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "standard_asset_events.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"
#include "x23/tests/death_test.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {
using default_wallet::WalletError;
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
  EXPECT_EQ(wallet_meta::method_id<u8"execute">(), 0xc3edb11fdd6d5d58ULL);
  EXPECT_EQ(wallet_meta::method_id<u8"deploy">(), 0x6be0cb0f6de0917aULL);
}

// Differential test: the native default_wallet precompile vs the rv32
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
          TestDB::kHogDb)
  {}

protected:
  static constexpr int32_t kNativeWalletId = 60;
  static constexpr int32_t kDriverBaseId = 61;
  static constexpr ContractAddress kEchoAddr = make_contract_address(220);
  static constexpr ContractAddress kAbortAddr = make_contract_address(221);
  static constexpr ContractAddress kMissingAddr = make_contract_address(240);

  PubkeyAccountPreimage native_preimage_{};
  PubkeyAccountPreimage rv32_preimage_{};
  ContractAddress native_wallet_{};
  ContractAddress rv32_wallet_{};
  EphemeralTxIdentifier last_tx_id_{};
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
    native_map.emplace(kNativeWalletId, make_native_default_wallet_contract());
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractHogToken),
        make_native_token_contract());
    for (auto &[id, c] : extra) {
      native_map.emplace(id, std::move(c));
    }
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    ctx_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });
  }

  void setup_wallets_and_targets()
  {
    native_preimage_ = make_preimage(1);
    rv32_preimage_ = make_preimage(2);
    // .value() throws on the (unreachable) empty case: pubkey_auth_address
    // only fails if the preimage cannot be serialized.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    native_wallet_ = pubkey_auth_address(native_preimage_).value();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    rv32_wallet_ = pubkey_auth_address(rv32_preimage_).value();

    auto native_obj = make_native_contract(kNativeWalletId);
    write_contract(native_obj);
    write_deployment(native_wallet_, native_obj);
    setup_grant_storage_token(native_wallet_, 1'000'000'000);

    auto rv32_obj = wallet_meta::load_rv32();
    ASSERT_TRUE(rv32_obj);
    write_contract(rv32_obj);
    write_deployment(rv32_wallet_, rv32_obj);
    setup_grant_storage_token(rv32_wallet_, 1'000'000'000);

    auto echo_obj = echo_meta::load_rv32();
    ASSERT_TRUE(echo_obj);
    write_contract(echo_obj);
    write_deployment(kEchoAddr, echo_obj);
    setup_grant_storage_token(kEchoAddr, 1'000'000'000);

    auto abort_obj = mrt_meta::load_rv32();
    ASSERT_TRUE(abort_obj);
    write_contract(abort_obj);
    write_deployment(kAbortAddr, abort_obj);
    setup_grant_storage_token(kAbortAddr, 1'000'000'000);

    auto token = make_native_contract(std::to_underlying(
        HogexecPrecompiledNativeContracts::kNativeContractHogToken));
    write_contract(token);
    write_deployment(kNativeTokenContractAddress, token);
  }

  static AuthClaim make_pubkey_claim(PubkeyAccountPreimage const &preimage)
  {
    AuthClaim claim{};
    claim.claim_type(AuthClaimType::kPubkeySignature);
    claim.pubkey_preimage() = preimage;
    return claim;
  }

  static WalletCall echo_call(size_t payload_len,
                              uint64_t gas_limit = UINT64_MAX)
  {
    x23::Vec<uint8_t, 4294967295> payload;
    payload.resize(payload_len, 0xAB);
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
  static WalletCall abort_call()
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
  CapturedRecord run_wallet_root_tx(Invocation invocation, AuthClaim claim,
                                    bool finish_block = true)
  {
    SignedTransaction stx;
    stx.tx.invocation = std::move(invocation);
    stx.tx.auth_claim = std::move(claim);
    stx.tx.uniquifier = make_new_uniquifier();

    auto handler = make_new_system_call_handler(stx, *ctx_, &last_tx_id_);
    auto res = handler.root_invoke()();
    CapturedRecord rec;
    rec.push_back(res.is_root_success() ? 1 : 0);
    auto frame_output = handler.get_most_recent_frame_output();
    ExecResult commit = handler.try_commit_transaction()();
    // Included transactions must flush events before the handler is destroyed.
    if (commit == ExecResult::ExecResultOK ||
        commit == ExecResult::ExecResultTxRootFrameReverted ||
        commit == ExecResult::ExecResultTxDeterministicError) {
      EXPECT_TRUE(
          handler.emit_events(commit, ProposedTransaction{.signed_tx = stx})());
    }
    rec.push_back(static_cast<uint8_t>(commit));
    switch (frame_output.status_code()) {
    case InvocationFrameStatusCode::Return:
    {
      auto const &buf = frame_output.return_buffer();
      rec.insert(rec.end(), buf.begin(), buf.end());
      break;
    }
    case InvocationFrameStatusCode::Abort:
    {
      auto const &buf = frame_output.abort_buffer();
      rec.insert(rec.end(), buf.begin(), buf.end());
      break;
    }
    case InvocationFrameStatusCode::Fault:
      rec.push_back(0xFF);
      break;
    }
    if (finish_block) {
      advance_block();
    }
    return rec;
  }

  CapturedRecord
  run_wallet_execute_tx(ContractAddress const &wallet,
                        PubkeyAccountPreimage const &preimage,
                        wallet_calls_t const &calls,
                        std::optional<AuthClaim> claim_override = std::nullopt)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet);
    return run_wallet_root_tx(client.make_invocation<u8"execute">(UINT64_MAX,
                                                                  calls),
                              claim_override ? *claim_override
                                             : make_pubkey_claim(preimage));
  }

  // Root-invokes `wallet`'s entrypoint with hand-crafted raw calldata (for
  // malformed-encoding cases that make_calldata cannot produce).
  CapturedRecord run_wallet_raw_tx(ContractAddress const &wallet,
                                   PubkeyAccountPreimage const &preimage,
                                   Runtime::MethodID method_id,
                                   std::vector<uint8_t> const &raw_calldata)
  {
    Invocation invocation{.invoked_address = wallet,
                          .entrypoint = entrypoint_from_method_id(method_id),
                          .gas_limit = UINT64_MAX,
                          .calldata = {}};
    invocation.calldata.append(raw_calldata.begin(), raw_calldata.end());
    return run_wallet_root_tx(std::move(invocation),
                              make_pubkey_claim(preimage));
  }

  // Runs the same raw calldata through both wallets and byte-compares.
  void expect_raw_parity(Runtime::MethodID method_id,
                         std::vector<uint8_t> const &raw_calldata)
  {
    auto native = run_wallet_raw_tx(native_wallet_, native_preimage_, method_id,
                                    raw_calldata);
    auto rv32 = run_wallet_raw_tx(rv32_wallet_, rv32_preimage_, method_id,
                                  raw_calldata);
    EXPECT_EQ(native, rv32);
  }

  // cxs(kWalletExceptionSelector, wallet_error): abort payload the wallet
  // emits for WalletError reverts (mirrors ContractException<WalletError,
  // 0x931d3297d37d1bdfULL>::serialize() in the generated SDK trait).
  static std::vector<uint8_t>
  wallet_error_abort_bytes(default_wallet::WalletError err)
  {
    constexpr uint64_t kWalletExceptionSelector = 0x931d3297d37d1bdfULL;
    auto buf = x23::serialize_cxs(kWalletExceptionSelector, err);
    CHECK(buf.has_value());
    return {buf->begin(), buf->end()};
  }

  // The expected abort payload for calldata the entrypoint glue rejects:
  // cxs(kLibraryExceptionSelector, LibraryExn::Handling(DecodingArgs)).
  static std::vector<uint8_t> decoding_args_abort_bytes()
  {
    LibraryExn exn(LibraryExn::kLibraryExnHandling);
    exn.handling() = HandlingError(HandlingError::kHandlingDecodingArgs);
    auto buf = x23::serialize_cxs(kLibraryExceptionSelector, exn);
    CHECK(buf.has_value());
    return {buf->begin(), buf->end()};
  }

  // Runs the same call list through both wallets and byte-compares.
  void expect_parity(wallet_calls_t const &calls)
  {
    auto native =
        run_wallet_execute_tx(native_wallet_, native_preimage_, calls);
    auto rv32 = run_wallet_execute_tx(rv32_wallet_, rv32_preimage_, calls);
    EXPECT_EQ(native, rv32);
  }

  // expect_parity, plus an absolute success check on the native record so a
  // shared-failure regression cannot pass vacuously (both backends faulting
  // identically would otherwise still satisfy EXPECT_EQ).
  void expect_parity_success(wallet_calls_t const &calls)
  {
    auto native =
        run_wallet_execute_tx(native_wallet_, native_preimage_, calls);
    auto rv32 = run_wallet_execute_tx(rv32_wallet_, rv32_preimage_, calls);
    EXPECT_EQ(native, rv32);
    ASSERT_GE(native.size(), 2U);
    EXPECT_EQ(native[0], 1);
    EXPECT_EQ(native[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
  }

  // Builds a native "driver" contract whose method 0 invokes `target`'s
  // `execute` entrypoint with `calls`, then records (tag, frame_status,
  // literal return bytes) into `result_key`. Adapted from erc20_parity_test.cc.
  static NativeContract::function_t make_wallet_capture_driver(
      ContractAddress target, StorageKeyContractData result_key,
      wallet_calls_t const &calls, bool mint_and_forward_token = false)
  {
    auto calldata = wallet_meta::make_calldata<u8"execute">(calls);
    Runtime::MethodID method_id = wallet_meta::method_id<u8"execute">();
    return [target, result_key, method_id, mint_and_forward_token,
            calldata = std::move(calldata)](
               NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
      // Optionally mint an auth token and forward its handle: execute takes
      // no Signer args, so the wallet must reject the unconsumed handle.
      uint32_t token_id = 0;
      std::span<uint32_t> forwarded{};
      if (mint_and_forward_token) {
        AuthTokenNode root_node{.consumer_address = target,
                                .payload = x23::serialize(1U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        if (!mint_res.has_value() ||
            mint_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "MintAuthToken failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        forwarded = std::span<uint32_t>{&token_id, 1};
      }

      CopyableInvokeOutputs outputs{};
      CopyableInvokeInputs inputs{
          .entrypoint_id = method_id,
          .gas_limit = static_cast<GasAmount>(UINT64_MAX),
          .flags = std::to_underlying(InvokeFlag::None),
      };
      auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
          target, std::as_bytes(std::span(calldata.data(), calldata.size())),
          forwarded, inputs, outputs);

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
          record.insert(record.end(), buf->begin(), buf->end());
        }
        else {
          record.push_back(0xFF);
        }
      }

      auto set_r = co_await ctx.syscall<SyscallNo::kBytestringSet>(
          result_key, std::as_bytes(std::span(record.data(), record.size())));
      if (!set_r.has_value() ||
          set_r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        ADD_FAILURE() << "capture driver failed to persist result";
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
      co_return InvokeStatus<void>{};
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

  CapturedRecord run_wallet_deploy_tx(ContractAddress const &wallet,
                                      PubkeyAccountPreimage const &preimage,
                                      SHA256Hash const &nonce,
                                      SHA256Hash const &hash)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet);
    return run_wallet_root_tx(client.make_invocation<u8"deploy">(
                                  UINT64_MAX, nonce, hash,
                                  std::tuple<int64_t, uint64_t>{0, 0},
                                  x23::Ptr<default_wallet::WalletInitCall>{}),
                              make_pubkey_claim(preimage));
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
    EXPECT_TRUE(handler.root_invoke()().is_root_success());
    ExecResult commit = handler.try_commit_transaction()();
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
  auto rv32 =
      run_wallet_execute_tx(rv32_wallet_, rv32_preimage_, calls, AuthClaim{});
  EXPECT_EQ(native, rv32);
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
  auto rv32 = run_wallet_execute_tx(rv32_wallet_, rv32_preimage_, calls, other);
  EXPECT_EQ(native, rv32);
}

// Non-root invocation (rv32 reference): a native driver invokes the rv32
// wallet; the rv32 reference rejects with NotRootInvocation. The native
// wallet's root guard is now a CHECK (resolution enforces root-only
// reachability), so the native side is tested separately as a death test
// (NonRootInvocationNativeChecks below).
TEST_F(DefaultWalletParityTest, NonRootInvocationRv32ReferenceRejects)
{
  setup_wallets_and_targets();

  wallet_calls_t calls;
  calls.push_back(echo_call(4));

  static constexpr ContractAddress kRv32DriverAddr = make_contract_address(231);
  auto rv32_result_key = make_storage_key("rv32_result");

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra;
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, make_wallet_capture_driver(rv32_wallet_, rv32_result_key,
                                              calls));
    extra.emplace(kDriverBaseId + 1,
                  std::make_unique<NativeContract>(std::move(fns)));
  }
  make_ctx(std::move(extra));

  auto obj = make_native_contract(kDriverBaseId + 1);
  write_contract(obj);
  write_deployment(kRv32DriverAddr, obj);
  setup_grant_storage_token(kRv32DriverAddr, 1'000'000'000);

  auto rv32_record = run_capture_tx(kRv32DriverAddr, rv32_result_key, *ctx_);

  // Record layout from run_capture_tx + make_wallet_capture_driver:
  // [commit, 0x00=invoke-accepted, frame_status, abort_payload].
  auto expected =
      wallet_error_abort_bytes(default_wallet::WalletError::NotRootInvocation);
  ASSERT_EQ(rv32_record.size(), 3U + expected.size());
  EXPECT_EQ(rv32_record[0], static_cast<uint8_t>(ExecResult::ExecResultOK));
  EXPECT_EQ(rv32_record[1], 0x00); // invoke succeeded, frame ran
  EXPECT_EQ(rv32_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_TRUE(
      std::equal(expected.begin(), expected.end(), rv32_record.begin() + 3));
}

// Non-root invocation (native): the native wallet's root guard is now a CHECK
// rather than a NotRootInvocation revert, because resolution enforces
// root-only reachability (no deployment row, hash banned from kDeployContract,
// resolve_runtime only falls back on an empty frame stack). A nested wallet
// frame is handler-level invariant breakage; verify the process dies.
TEST_F(DefaultWalletParityTest, NonRootInvocationNativeChecks)
{
  if constexpr (!is_debug_build()) {
    // The "CHECK FAILED" panic message this death test matches is only
    // produced in debug builds (x23::panic is std::unreachable() under
    // NDEBUG).  Matches RecoveryTests.RecoveredHighBitRCQLiveEntriesDiesOnLoad
    // and IndexDBTest.GetEventsShadowedReceiptBlockMismatchIsFatal.  Without
    // this skip the release ASAN build reaches std::unreachable() and dies of
    // a SEGV instead, so the expected-message match fails.
    GTEST_SKIP();
  }

  setup_wallets_and_targets();

  wallet_calls_t calls;
  calls.push_back(echo_call(4));

  static constexpr ContractAddress kNativeDriverAddr =
      make_contract_address(230);
  auto native_result_key = make_storage_key("native_result");

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra;
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, make_wallet_capture_driver(native_wallet_, native_result_key,
                                              calls));
    extra.emplace(kDriverBaseId,
                  std::make_unique<NativeContract>(std::move(fns)));
  }
  make_ctx(std::move(extra));

  auto obj = make_native_contract(kDriverBaseId);
  write_contract(obj);
  write_deployment(kNativeDriverAddr, obj);
  setup_grant_storage_token(kNativeDriverAddr, 1'000'000'000);

  GTEST_FLAG_SET(death_test_style, "threadsafe");
  X23_EXPECT_DEATH(run_capture_tx(kNativeDriverAddr, native_result_key, *ctx_),
                   "CHECK FAILED");
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
    // item_len = 1 + 11 = 12; 16 calls: 1 + 16*12 = 193 bytes <
    // kMaxReturnDataBytes
    calls.push_back(echo_call(10));
  }
  expect_parity_success(calls);
}

TEST_F(DefaultWalletParityTest, MultiCallHappyPathReturnsEchoedBytes)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(1));
  calls.push_back(echo_call(30));
  calls.push_back(echo_call(0)); // zero-length echo: item is prefix-only
  expect_parity_success(calls);
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

TEST_F(DefaultWalletParityTest, ReturnDataExactFitSucceeds)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(1017)); // total = exactly 1024
  expect_parity_success(calls);
}

TEST_F(DefaultWalletParityTest, ReturnDataOneOverFails)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(1018)); // total = 1025 -> ReturnDataTooLarge
  auto native = run_wallet_execute_tx(native_wallet_, native_preimage_, calls);
  auto rv32 = run_wallet_execute_tx(rv32_wallet_, rv32_preimage_, calls);
  EXPECT_EQ(native, rv32);
  // root_success=1 for Abort (only Fault yields 0); the revert is signalled
  // by the commit result: ExecResultTxRootFrameReverted confirms the wallet
  // aborted and no state was committed.
  ASSERT_GE(native.size(), 2U);
  EXPECT_EQ(native[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
}

TEST_F(DefaultWalletParityTest, ReturnDataMaxSingleCalleeReturnFails)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(1021)); // n = 1024 (callee max), total = 1028
  expect_parity(calls);
}

// Overflow triggered on the SECOND call: the first call's result must be
// discarded by the revert identically on both backends.
TEST_F(DefaultWalletParityTest, ReturnDataCumulativeOverflowOnLaterCall)
{
  setup_wallets_and_targets();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(600));
  calls.push_back(echo_call(600));
  expect_parity(calls);
}

// CXS length-prefix width breakpoints: where cxs_u32_len(n) jumps 1->2 and
// 2->3 — the spots where a hand-mirrored accounting would drift.
TEST_F(DefaultWalletParityTest, ReturnDataPrefixWidthBreakpoints)
{
  setup_wallets_and_targets();
  make_ctx();
  for (size_t d : {60U, 61U, 62U, 63U, 64U, 252U, 253U, 254U, 255U, 256U}) {
    wallet_calls_t calls;
    calls.push_back(echo_call(d));
    SCOPED_TRACE(d);
    expect_parity(calls);
  }
}

TEST_F(DefaultWalletParityTest, DeployHappyPath)
{
  setup_wallets_and_targets();
  make_ctx();
  // Deployable target: any contract present in the DB by hash. echo is
  // already written by setup; reuse its hash.
  auto echo_obj = echo_meta::load_rv32();
  SHA256Hash target_hash = sha256(*echo_obj);
  SHA256Hash nonce{};
  nonce.fill(9);

  auto native = run_wallet_deploy_tx(native_wallet_, native_preimage_, nonce,
                                     target_hash);
  auto rv32 =
      run_wallet_deploy_tx(rv32_wallet_, rv32_preimage_, nonce, target_hash);

  // Status + commit parity (indices per run_wallet_root_tx record layout).
  ASSERT_GE(native.size(), 2U);
  ASSERT_GE(rv32.size(), 2U);
  EXPECT_EQ(native[0], rv32[0]);
  EXPECT_EQ(native[1], rv32[1]);
  EXPECT_EQ(native[0], 1);

  // Each backend's returned bytes are its own expected deploy address
  // (CXS of a fixed 32-byte array: the raw bytes, no prefix).
  auto expect_addr = [](CapturedRecord const &rec,
                        ContractAddress const &want) {
    ASSERT_EQ(rec.size(), 2U + 32U);
    EXPECT_TRUE(std::equal(want.begin(), want.end(), rec.begin() + 2));
  };
  expect_addr(native,
              SystemCallHandler::static_get_deploy_address(native_wallet_,
                                                           nonce, target_hash));
  expect_addr(rv32,
              SystemCallHandler::static_get_deploy_address(rv32_wallet_, nonce,
                                                           target_hash));
}

TEST_F(DefaultWalletParityTest, FundedDeploySuccessAndDuplicateAcrossBlocks)
{
  setup_wallets_and_targets();
  make_ctx();
  auto code = echo_meta::load_rv32();
  ASSERT_TRUE(code);
  auto hash = sha256(*code);
  SHA256Hash nonce{};
  nonce.fill(42);
  constexpr uint64_t kAmount = 10'000'000;
  auto calldata = wallet_meta::make_calldata<u8"deploy">(
      nonce, hash, std::tuple<int64_t, uint64_t>{0, kAmount},
      x23::Ptr<default_wallet::WalletInitCall>{});
  std::vector<uint8_t> raw(calldata.begin(), calldata.end());
  auto id = wallet_meta::method_id<u8"deploy">();
  auto stored_balance = [this](ContractAddress const &address) -> int128_t {
    auto item = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(address));
    return item ? from_xdr(item->nnint()) : 0;
  };
  CapturedRecord duplicate_record;
  for (auto const &[wallet, preimage] : {
           std::pair{native_wallet_, native_preimage_},
           std::pair{rv32_wallet_, rv32_preimage_},
       }) {
    SCOPED_TRACE(wallet == native_wallet_ ? "native" : "rv32");
    auto target =
        SystemCallHandler::static_get_deploy_address(wallet, nonce, hash);
    auto sender_before = stored_balance(wallet);
    auto block = get_block_number();
    auto record = run_wallet_raw_tx(wallet, preimage, id, raw);
    ASSERT_EQ(record.size(), 34U);
    ASSERT_EQ(record[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
    EXPECT_TRUE(std::equal(target.begin(), target.end(), record.begin() + 2));
    auto deployed = lookup_deployment_from_db(target);
    if (!deployed.has_value()) {
      ADD_FAILURE() << "expected the deployment to be committed";
      return;
    }
    EXPECT_EQ(deployed->hash, hash);
    auto target_balance =
        int128_t{kAmount} - get_default_cost_model()->get_balance_cost() -
        get_default_cost_model()->get_deployment_cost(*deployed);
    EXPECT_EQ(stored_balance(wallet),
              sender_before - kAmount - replay_cache_cost_charged(1));
    EXPECT_EQ(stored_balance(target), target_balance);
    auto deltas = get_hogs_transferred(block);
    ASSERT_EQ(deltas.size(), 2U);
    EXPECT_EQ(deltas.at(wallet),
              -int128_t{kAmount} - replay_cache_cost_charged(1));
    EXPECT_EQ(deltas.at(target), target_balance);
    auto events = get_tx_event_log(last_tx_id_)();
    if (!events.has_value()) {
      ADD_FAILURE() << "expected a transaction event log";
      return;
    }
    ASSERT_EQ(events->events.size(), 1U);
    EXPECT_EQ(events->events[0].contract_address, kNativeTokenContractAddress);
    auto expected_event =
        make_event_from_cxs_struct(native_token_transfer_event_signature(),
                                   TransferEvent{
                                       .from_address = wallet,
                                       .to_address = target,
                                       .amount = to_xdr(int128_t{kAmount}),
                                   });
    ASSERT_TRUE(expected_event);
    EXPECT_TRUE(std::ranges::equal(events->events[0].serialized_event,
                                   *expected_event));

    // Once committed in an earlier block, both wallets reject the deployment
    // with the same exception and roll back the second transfer. Only the
    // ordinary replay-cache collateral charge remains.
    sender_before = stored_balance(wallet);
    block = get_block_number();
    auto duplicate = run_wallet_raw_tx(wallet, preimage, id, raw);
    ASSERT_GE(duplicate.size(), 2U);
    EXPECT_EQ(duplicate[1],
              static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
    if (duplicate_record.empty()) {
      duplicate_record = duplicate;
    }
    else {
      EXPECT_EQ(duplicate, duplicate_record);
    }
    auto replay_delta =
        replay_cache_cost_charged(2) - replay_cache_cost_charged(1);
    EXPECT_EQ(stored_balance(wallet), sender_before - replay_delta);
    EXPECT_EQ(stored_balance(target), target_balance);
    EXPECT_TRUE(lookup_deployment_from_db(target));
    auto duplicate_deltas = get_hogs_transferred(block);
    ASSERT_EQ(duplicate_deltas.size(), 1U);
    EXPECT_EQ(duplicate_deltas.at(wallet), -replay_delta);
    auto duplicate_events = get_tx_event_log(last_tx_id_)();
    if (!duplicate_events.has_value()) {
      ADD_FAILURE() << "expected a transaction event log";
      return;
    }
    EXPECT_TRUE(duplicate_events->events.empty());
  }
}

TEST_F(DefaultWalletParityTest, SameBlockDuplicateDeployFundsTwice)
{
  setup_wallets_and_targets();
  make_ctx();
  auto code = echo_meta::load_rv32();
  ASSERT_TRUE(code);
  auto hash = sha256(*code);
  SHA256Hash nonce{};
  nonce.fill(43);
  constexpr uint64_t kAmount = 10'000'000;
  auto stored_balance = [this](ContractAddress const &address) -> int128_t {
    auto item = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(address));
    return item ? from_xdr(item->nnint()) : 0;
  };
  for (auto const &[wallet, preimage] : {
           std::pair{native_wallet_, native_preimage_},
           std::pair{rv32_wallet_, rv32_preimage_},
       }) {
    SCOPED_TRACE(wallet == native_wallet_ ? "native" : "rv32");
    auto target =
        SystemCallHandler::static_get_deploy_address(wallet, nonce, hash);
    EXPECT_FALSE(lookup_deployment_from_db(target));
    auto sender_before = stored_balance(wallet);
    auto block = get_block_number();
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet);
    std::vector<EphemeralTxIdentifier> tx_ids;
    for (int i = 0; i < 2; ++i) {
      // Each call gets a fresh transaction replay nonce, but both deployment
      // nonces and hashes match. The block snapshot still has no deployment.
      auto record =
          run_wallet_root_tx(client.make_invocation<u8"deploy">(
                                 UINT64_MAX, nonce, hash,
                                 std::tuple<int64_t, uint64_t>{0, kAmount},
                                 x23::Ptr<default_wallet::WalletInitCall>{}),
                             make_pubkey_claim(preimage),
                             /*finish_block=*/false);
      ASSERT_EQ(record.size(), 34U);
      EXPECT_EQ(record[0], 1);
      ASSERT_EQ(record[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
      EXPECT_TRUE(std::equal(target.begin(), target.end(), record.begin() + 2));
      EXPECT_EQ(get_block_number(), block);
      tx_ids.push_back(last_tx_id_);
    }
    EXPECT_EQ(tx_ids[0].block_id, tx_ids[1].block_id);
    ASSERT_NE(tx_ids[0].tx_id, tx_ids[1].tx_id);
    advance_block();

    auto deployed = lookup_deployment_from_db(target);
    if (!deployed.has_value()) {
      ADD_FAILURE() << "expected the deployment to be committed";
      return;
    }
    EXPECT_EQ(deployed->hash, hash);
    // The two transfers accumulate, while the single deployment and balance
    // entry incur collateral only once.
    auto target_balance =
        (2 * int128_t{kAmount}) - get_default_cost_model()->get_balance_cost() -
        get_default_cost_model()->get_deployment_cost(*deployed);
    auto sender_delta = (-2 * int128_t{kAmount}) - replay_cache_cost_charged(2);
    EXPECT_EQ(stored_balance(wallet), sender_before + sender_delta);
    EXPECT_EQ(stored_balance(target), target_balance);
    auto deltas = get_hogs_transferred(block);
    ASSERT_EQ(deltas.size(), 2U);
    EXPECT_EQ(deltas.at(wallet), sender_delta);
    EXPECT_EQ(deltas.at(target), target_balance);

    auto expected_event =
        make_event_from_cxs_struct(native_token_transfer_event_signature(),
                                   TransferEvent{
                                       .from_address = wallet,
                                       .to_address = target,
                                       .amount = to_xdr(int128_t{kAmount}),
                                   });
    ASSERT_TRUE(expected_event);
    for (auto const &tx_id : tx_ids) {
      auto events = get_tx_event_log(tx_id)();
      if (!events.has_value()) {
        ADD_FAILURE() << "expected a transaction event log";
        return;
      }
      ASSERT_EQ(events->events.size(), 1U);
      EXPECT_EQ(events->events[0].contract_address,
                kNativeTokenContractAddress);
      EXPECT_TRUE(std::ranges::equal(events->events[0].serialized_event,
                                     *expected_event));
    }
  }
}

// Drive both wallet implementations through an initializer that records its
// caller and calldata, then returns, aborts or faults after writing state.
TEST_F(DefaultWalletParityTest, DeploymentInitializationCommitsOrRollsBack)
{
  setup_wallets_and_targets();
  make_ctx();
  auto key = make_storage_key_matching_static_key_sdk_macro("initialized");
  auto code = mrt_meta::load_rv32();
  ASSERT_TRUE(code);
  auto hash = sha256(*code);
  auto stored_balance = [this](ContractAddress const &address) -> int128_t {
    auto item = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(address));
    return item ? from_xdr(item->nnint()) : 0;
  };
  for (auto const &[wallet, preimage] : {
           std::pair{native_wallet_, native_preimage_},
           std::pair{rv32_wallet_, rv32_preimage_},
       }) {
    SCOPED_TRACE(wallet == native_wallet_ ? "native" : "rv32");
    for (uint8_t mode = 0; mode < 4; ++mode) {
      SCOPED_TRACE(mode);
      SHA256Hash nonce{};
      nonce[0] = mode;
      auto target =
          SystemCallHandler::static_get_deploy_address(wallet, nonce, hash);
      // Last case proves zero funding plus initialization uses existing funds
      // without emitting even a zero-valued token transfer.
      uint64_t amount = mode == 3 ? 0 : 10'000'000;
      if (amount == 0) {
        setup_grant_storage_token(target, 10'000'000);
      }
      default_wallet::WalletInitCall init{};
      init.entrypoint = mrt_meta::entrypoint<u8"test_initializer">();
      init.gas_limit = UINT64_MAX;
      auto args = mrt_meta::make_calldata<u8"test_initializer">(
          wallet, uint32_t{0x1234}, uint32_t{mode == 3 ? 0U : mode});
      init.calldata.append_range(args);
      auto raw = wallet_meta::make_calldata<u8"deploy">(
          nonce, hash, std::tuple<int64_t, uint64_t>{0, amount},
          x23::Ptr<default_wallet::WalletInitCall>{init});
      auto before = stored_balance(wallet);
      auto record = run_wallet_raw_tx(wallet, preimage,
                                      wallet_meta::method_id<u8"deploy">(),
                                      {raw.begin(), raw.end()});
      ASSERT_GE(record.size(), 2U);
      auto replay_delta = replay_cache_cost_charged(mode + 1) -
                          (mode == 0 ? 0 : replay_cache_cost_charged(mode));
      auto events = get_tx_event_log(last_tx_id_)();
      if (!events.has_value()) {
        ADD_FAILURE() << "expected included transaction event log";
        return;
      }
      if (mode == 0 || mode == 3) {
        ASSERT_EQ(record[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
        ASSERT_EQ(record.size(), 34U);
        EXPECT_TRUE(
            std::equal(target.begin(), target.end(), record.begin() + 2));
        EXPECT_TRUE(lookup_deployment_from_db(target));
        auto state = read_record(target, key);
        std::vector<uint8_t> expected(wallet.begin(), wallet.end());
        expected.insert(expected.end(), {0, 0, 0x12, 0x34});
        EXPECT_EQ(state, expected);
        EXPECT_EQ(stored_balance(wallet), before - amount - replay_delta);
        EXPECT_EQ(events->events.size(), amount == 0 ? 0U : 1U);
      }
      else {
        EXPECT_EQ(record[1], static_cast<uint8_t>(
                                 ExecResult::ExecResultTxRootFrameReverted));
        auto expected = wallet_error_abort_bytes(
            mode == 1 ? WalletError::CallAborted : WalletError::CallFaulted);
        ASSERT_EQ(record.size(), 2U + expected.size());
        EXPECT_TRUE(
            std::equal(expected.begin(), expected.end(), record.begin() + 2));
        EXPECT_EQ(stored_balance(wallet), before - replay_delta);
        EXPECT_FALSE(lookup_deployment_from_db(target));
        EXPECT_FALSE(lookup_storage_item_from_db(
            make_free_native_token_balance_addrkey(target)));
        StorageKey storage_key(StorageKeyType::ContractData);
        storage_key.contract_data_key() = key;
        EXPECT_FALSE(lookup_storage_item_from_db(
            AddressAndKey{.address = target, .key = storage_key}));
        EXPECT_TRUE(events->events.empty());
      }
    }
  }
}

TEST_F(DefaultWalletParityTest, DeploymentInitializesRv32CodeWithCallerGasLimit)
{
  setup_wallets_and_targets();
  make_ctx();
  auto code = mrt_meta::load_rv32();
  ASSERT_TRUE(code);
  auto hash = sha256(*code);
  for (auto const &[wallet, preimage] : {
           std::pair{native_wallet_, native_preimage_},
           std::pair{rv32_wallet_, rv32_preimage_},
       }) {
    SCOPED_TRACE(wallet == native_wallet_ ? "native" : "rv32");
    for (uint8_t mode = 0; mode < 3; ++mode) {
      SCOPED_TRACE(mode);
      SHA256Hash nonce{};
      nonce[0] = mode;
      auto target =
          SystemCallHandler::static_get_deploy_address(wallet, nonce, hash);
      default_wallet::WalletInitCall init{};
      init.entrypoint = mrt_meta::entrypoint<u8"test_basic_manual_revert">();
      init.gas_limit = mode == 2 ? 0 : UINT64_MAX;
      auto args =
          mrt_meta::make_calldata<u8"test_basic_manual_revert">(mode == 1);
      init.calldata.append_range(args);
      auto raw = wallet_meta::make_calldata<u8"deploy">(
          nonce, hash, std::tuple<int64_t, uint64_t>{0, 10'000'000},
          x23::Ptr<default_wallet::WalletInitCall>{init});
      auto record = run_wallet_raw_tx(wallet, preimage,
                                      wallet_meta::method_id<u8"deploy">(),
                                      {raw.begin(), raw.end()});
      ASSERT_GE(record.size(), 2U);
      if (mode == 0) {
        ASSERT_EQ(record[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
        ASSERT_EQ(record.size(), 34U);
        EXPECT_TRUE(
            std::equal(target.begin(), target.end(), record.begin() + 2));
        WalletCall query{};
        query.target = target;
        query.entrypoint = mrt_meta::entrypoint<u8"check_storage_exists">();
        query.gas_limit = UINT64_MAX;
        auto returned = run_wallet_execute_tx(wallet, preimage, {query});
        // One successful child returning CXS true: Vec<Vec<u8>> = [ [1] ].
        EXPECT_EQ(
            returned,
            (CapturedRecord{1, static_cast<uint8_t>(ExecResult::ExecResultOK),
                            1, 1, 1}));
      }
      else {
        auto expected = wallet_error_abort_bytes(
            mode == 1 ? WalletError::CallAborted : WalletError::CallFaulted);
        ASSERT_EQ(record.size(), 2U + expected.size());
        EXPECT_EQ(record[1], static_cast<uint8_t>(
                                 ExecResult::ExecResultTxRootFrameReverted));
        EXPECT_TRUE(
            std::equal(expected.begin(), expected.end(), record.begin() + 2));
        EXPECT_FALSE(lookup_deployment_from_db(target));
        EXPECT_FALSE(lookup_storage_item_from_db(
            make_free_native_token_balance_addrkey(target)));
        auto events = get_tx_event_log(last_tx_id_)();
        if (!events.has_value()) {
          ADD_FAILURE() << "expected reverted transaction event log";
          return;
        }
        EXPECT_TRUE(events->events.empty());
      }
    }
  }
}

TEST_F(DefaultWalletParityTest, FundedDeployFailureAndEncodingParity)
{
  setup_wallets_and_targets();
  make_ctx();
  auto code = echo_meta::load_rv32();
  ASSERT_TRUE(code);
  auto hash = sha256(*code);
  SHA256Hash nonce{};
  auto id = wallet_meta::method_id<u8"deploy">();
  for (auto amount : {
           int128_t{0},
           int128_t{1},
           int128_t{2'000'000'000},
           int128_t{-1},
           std::numeric_limits<int128_t>::min(),
       }) {
    SCOPED_TRACE(static_cast<int64_t>(amount >> 64U));
    SCOPED_TRACE(static_cast<uint64_t>(amount));
    auto calldata = wallet_meta::make_calldata<u8"deploy">(
        nonce, hash,
        std::tuple<int64_t, uint64_t>{static_cast<int64_t>(amount >> 64U),
                                      static_cast<uint64_t>(amount)},
        x23::Ptr<default_wallet::WalletInitCall>{});
    if (amount == 0 || amount == 1) {
      // Execution returns the per-wallet address, then collateral validation
      // rejects the commit. Compare each return with its own derived address.
      for (auto const &[wallet, preimage] : {
               std::pair{native_wallet_, native_preimage_},
               std::pair{rv32_wallet_, rv32_preimage_},
           }) {
        auto rec = run_wallet_raw_tx(wallet, preimage, id,
                                     {calldata.begin(), calldata.end()});
        ASSERT_EQ(rec.size(), 34U);
        EXPECT_EQ(rec[1], static_cast<uint8_t>(
                              ExecResult::ExecResultTxNonDeterministicError));
        auto target =
            SystemCallHandler::static_get_deploy_address(wallet, nonce, hash);
        EXPECT_TRUE(std::equal(target.begin(), target.end(), rec.begin() + 2));
        EXPECT_FALSE(lookup_deployment_from_db(target));
      }
    }
    else if (amount < 0) {
      // The wallet hands the negative amount to the token, which rejects it
      // with NegativeAmount: the wallet reverts with CallAborted, deploys
      // nothing and moves no tokens, identically on both wallets.
      auto const expected = wallet_error_abort_bytes(WalletError::CallAborted);
      for (auto const &[wallet, preimage] : {
               std::pair{native_wallet_, native_preimage_},
               std::pair{rv32_wallet_, rv32_preimage_},
           }) {
        SCOPED_TRACE(wallet == native_wallet_ ? "native" : "rv32");
        auto rec = run_wallet_raw_tx(wallet, preimage, id,
                                     {calldata.begin(), calldata.end()});
        ASSERT_EQ(rec.size(), 2U + expected.size());
        EXPECT_EQ(rec[1], static_cast<uint8_t>(
                              ExecResult::ExecResultTxRootFrameReverted));
        EXPECT_TRUE(
            std::equal(expected.begin(), expected.end(), rec.begin() + 2));
        auto target =
            SystemCallHandler::static_get_deploy_address(wallet, nonce, hash);
        EXPECT_FALSE(lookup_deployment_from_db(target));
        EXPECT_FALSE(lookup_storage_item_from_db(
            make_free_native_token_balance_addrkey(target)));
        auto events = get_tx_event_log(last_tx_id_)();
        ASSERT_TRUE(events.has_value());
        EXPECT_TRUE(events->events.empty());
      }
    }
    else {
      expect_raw_parity(id, {calldata.begin(), calldata.end()});
    }
    ++nonce[0];
  }
  SHA256Hash missing{};
  missing.fill(0xEE);
  auto calldata = wallet_meta::make_calldata<u8"deploy">(
      nonce, missing, std::tuple<int64_t, uint64_t>{0, 10'000'000},
      x23::Ptr<default_wallet::WalletInitCall>{});
  expect_raw_parity(id, {calldata.begin(), calldata.end()});
  expect_raw_parity(id, {});
  std::vector<uint8_t> truncated(calldata.begin(), calldata.end() - 1);
  expect_raw_parity(id, truncated);
  std::vector<uint8_t> trailing(calldata.begin(), calldata.end());
  trailing.push_back(0);
  expect_raw_parity(id, trailing);
}

TEST_F(DefaultWalletParityTest, DeployUnknownHashFaults)
{
  setup_wallets_and_targets();
  make_ctx();
  SHA256Hash bogus{};
  bogus.fill(0xEE);
  SHA256Hash nonce{};
  nonce.fill(10);
  auto native =
      run_wallet_deploy_tx(native_wallet_, native_preimage_, nonce, bogus);
  auto rv32 = run_wallet_deploy_tx(rv32_wallet_, rv32_preimage_, nonce, bogus);
  // Address-independent outcome: full byte comparison applies.
  EXPECT_EQ(native, rv32);
}

// Malformed calldata never reaches the method body: the sdk entrypoint glue
// aborts with LibraryExn::Handling(DecodingArgs), and the native builder
// must produce byte-identical aborts. User-reachable once the precompile is
// registered, so every malformed shape is pinned here.
TEST_F(DefaultWalletParityTest, MalformedCalldataExecute)
{
  setup_wallets_and_targets();
  make_ctx();
  auto exec_id = wallet_meta::method_id<u8"execute">();

  wallet_calls_t calls;
  calls.push_back(echo_call(4));
  auto valid = wallet_meta::make_calldata<u8"execute">(calls);
  std::vector<uint8_t> valid_bytes(valid.begin(), valid.end());

  {
    SCOPED_TRACE("empty");
    expect_raw_parity(exec_id, {});
  }
  {
    SCOPED_TRACE("truncated");
    auto truncated = valid_bytes;
    truncated.pop_back();
    expect_raw_parity(exec_id, truncated);
  }
  {
    SCOPED_TRACE("trailing");
    auto trailing = valid_bytes;
    trailing.push_back(0x00);
    expect_raw_parity(exec_id, trailing);
  }
  {
    SCOPED_TRACE("noncanonical-count-width");
    // Replace the call-count's 1-byte CXS prefix with the 4-byte integer
    // form (0xBF header + LE u32) encoding the same count. Whether decoders
    // accept the wide form or reject it, both backends must agree.
    std::vector<uint8_t> wide = {0xBF, 0x01, 0x00, 0x00, 0x00};
    wide.insert(wide.end(), valid_bytes.begin() + 1, valid_bytes.end());
    expect_raw_parity(exec_id, wide);
  }

  // Absolute check on one case: the abort payload is exactly the
  // DecodingArgs encoding, and the tx is a reverted root frame.
  auto native =
      run_wallet_raw_tx(native_wallet_, native_preimage_, exec_id, {});
  auto expected = decoding_args_abort_bytes();
  ASSERT_EQ(native.size(), 2U + expected.size());
  EXPECT_EQ(native[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), native.begin() + 2));
}

TEST_F(DefaultWalletParityTest, MalformedCalldataDeploy)
{
  setup_wallets_and_targets();
  make_ctx();
  auto deploy_id = wallet_meta::method_id<u8"deploy">();

  SHA256Hash nonce{};
  nonce.fill(9);
  SHA256Hash hash{};
  hash.fill(1);
  auto valid = wallet_meta::make_calldata<u8"deploy">(
      nonce, hash, std::tuple<int64_t, uint64_t>{0, 0},
      x23::Ptr<default_wallet::WalletInitCall>{});
  std::vector<uint8_t> valid_bytes(valid.begin(), valid.end());

  {
    SCOPED_TRACE("empty");
    expect_raw_parity(deploy_id, {});
  }
  {
    SCOPED_TRACE("truncated");
    auto truncated = valid_bytes;
    truncated.pop_back();
    expect_raw_parity(deploy_id, truncated);
  }
  {
    SCOPED_TRACE("trailing");
    auto trailing = valid_bytes;
    trailing.push_back(0x00);
    expect_raw_parity(deploy_id, trailing);
  }
}

// The sdk rejects auth handles left unconsumed by argument decoding
// (execute takes no Signer args), folding them into the same DecodingArgs
// abort. A driver mints a real token and forwards its handle to each
// wallet; both backends must reject identically, before the method runs.
TEST_F(DefaultWalletParityTest, ForwardedAuthHandleRejected)
{
  setup_wallets_and_targets();

  wallet_calls_t calls;
  calls.push_back(echo_call(4));

  static constexpr ContractAddress kNativeDriverAddr =
      make_contract_address(230);
  static constexpr ContractAddress kRv32DriverAddr = make_contract_address(231);
  auto native_result_key = make_storage_key("native_result");
  auto rv32_result_key = make_storage_key("rv32_result");

  auto make_driver = [&](ContractAddress const &wallet,
                         StorageKeyContractData const &result_key) {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, make_wallet_capture_driver(wallet, result_key, calls,
                                              /*mint_and_forward_token=*/true));
    return std::make_unique<NativeContract>(std::move(fns));
  };
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra;
  extra.emplace(kDriverBaseId, make_driver(native_wallet_, native_result_key));
  extra.emplace(kDriverBaseId + 1, make_driver(rv32_wallet_, rv32_result_key));
  make_ctx(std::move(extra));

  auto deploy_driver = [&](int32_t id, ContractAddress const &addr) {
    auto obj = make_native_contract(id);
    write_contract(obj);
    write_deployment(addr, obj);
    setup_grant_storage_token(addr, 1'000'000'000);
  };
  deploy_driver(kDriverBaseId, kNativeDriverAddr);
  deploy_driver(kDriverBaseId + 1, kRv32DriverAddr);

  auto native_record =
      run_capture_tx(kNativeDriverAddr, native_result_key, *ctx_);
  auto rv32_record = run_capture_tx(kRv32DriverAddr, rv32_result_key, *ctx_);
  EXPECT_EQ(native_record, rv32_record);

  // Absolute check: [commit, tag=0x00, frame_status=Abort, DecodingArgs].
  auto expected = decoding_args_abort_bytes();
  ASSERT_EQ(native_record.size(), 3U + expected.size());
  EXPECT_EQ(native_record[2],
            static_cast<uint8_t>(InvocationFrameStatusCode::Abort));
  EXPECT_TRUE(
      std::equal(expected.begin(), expected.end(), native_record.begin() + 3));
}

// echo_call(0) returns a 1-byte CXS encoding of an empty Vec<u8>, not an
// empty buffer. A raw native callee that sets no return buffer exercises
// the true outputs.return_buffer_len == 0 path.
TEST_F(DefaultWalletParityTest, ZeroByteChildReturn)
{
  setup_wallets_and_targets();

  static constexpr ContractAddress kEmptyReturnAddr =
      make_contract_address(222);
  constexpr int32_t kEmptyReturnId = 63;
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext & /*unused*/)
                  -> x23::Async<InvokeStatus<void>> {
                co_return InvokeStatus<void>{};
              });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra;
  extra.emplace(kEmptyReturnId,
                std::make_unique<NativeContract>(std::move(fns)));
  make_ctx(std::move(extra));
  auto obj = make_native_contract(kEmptyReturnId);
  write_contract(obj);
  write_deployment(kEmptyReturnAddr, obj);
  setup_grant_storage_token(kEmptyReturnAddr, 1'000'000'000);

  WalletCall call{};
  call.target = kEmptyReturnAddr;
  call.entrypoint = entrypoint_from_method_id(0);
  call.gas_limit = UINT64_MAX;
  wallet_calls_t calls;
  calls.push_back(call);
  calls.push_back(echo_call(4));
  expect_parity_success(calls);
}

// A callee that returns a minted auth handle: the rv32 wallet fetches the
// handle list and drops it, the native wallet passes an empty fetch span —
// the observable wallet behavior must match regardless.
TEST_F(DefaultWalletParityTest, CalleeReturnsAuthHandle)
{
  setup_wallets_and_targets();

  static constexpr ContractAddress kTokenReturnAddr =
      make_contract_address(223);
  constexpr int32_t kTokenReturnId = 64;
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kTokenReturnAddr,
                                .payload = x23::serialize(2U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        if (!mint_res.has_value() ||
            mint_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "MintAuthToken failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        ctx.handler().set_return_buffer(ReturnBuffer{0xAA, 0xBB}, false,
                                        {token_id});
        co_return InvokeStatus<void>{};
      });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> extra;
  extra.emplace(kTokenReturnId,
                std::make_unique<NativeContract>(std::move(fns)));
  make_ctx(std::move(extra));
  auto obj = make_native_contract(kTokenReturnId);
  write_contract(obj);
  write_deployment(kTokenReturnAddr, obj);
  setup_grant_storage_token(kTokenReturnAddr, 1'000'000'000);

  WalletCall call{};
  call.target = kTokenReturnAddr;
  call.entrypoint = entrypoint_from_method_id(0);
  call.gas_limit = UINT64_MAX;
  wallet_calls_t calls;
  calls.push_back(call);
  expect_parity_success(calls);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
