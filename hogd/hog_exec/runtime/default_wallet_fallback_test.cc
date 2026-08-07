#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "built_contracts/calldata_wrapper.h"
#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/item/native_contract_record.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {
using wallet_meta = ContractTraitMeta<default_wallet::DefaultWallet>;
using echo_meta = ContractTraitMeta<calldata_wrapper::CalldataWrapper>;
using default_wallet::WalletCall;
using default_wallet::WalletError;
using CapturedRecord = std::vector<uint8_t>;
using wallet_calls_t = x23::Vec<WalletCall, 4294967295>;
} // namespace

// Root-fallback integration suite: a root transaction addressed to an
// UNDEPLOYED pubkey-auth address resolves the default_wallet precompile via
// resolve_runtime, exercising the path added in Task 4.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class DefaultWalletFallbackTest : public ::testing::Test,
                                  public NativeContractTestFramework {
public:
  DefaultWalletFallbackTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  // Production native contract id — the key genesis writes and the key
  // make_root_fallback_wallet_runtime() resolves against in NativeContext.
  static constexpr int32_t kNativeWalletId = std::to_underlying(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet);
  static constexpr ContractAddress kEchoAddr = make_contract_address(220);

  PubkeyAccountPreimage preimage_{};
  ContractAddress wallet_{};
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

  static AuthClaim make_pubkey_claim(PubkeyAccountPreimage const &preimage)
  {
    AuthClaim claim{};
    claim.claim_type(AuthClaimType::kPubkeySignature);
    claim.pubkey_preimage() = preimage;
    return claim;
  }

  // ctx_ with the wallet registered under its production id.
  void make_ctx()
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(kNativeWalletId, make_native_default_wallet_contract());
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    ctx_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });
  }

  // Writes the wallet contract record and the echo target. Does NOT
  // write_deployment for wallet_ — resolution exercises the undeployed path.
  void setup_wallet_and_echo()
  {
    preimage_ = make_preimage(1);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    wallet_ = pubkey_auth_address(preimage_).value();

    write_contract(make_native_contract_record(
        HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet));
    setup_grant_storage_token(wallet_, 1'000'000'000);

    auto echo_obj = echo_meta::load_wasm();
    ASSERT_TRUE(echo_obj);
    write_contract(echo_obj);
    write_deployment(kEchoAddr, echo_obj);
    setup_grant_storage_token(kEchoAddr, 1'000'000'000);
  }

  // [root_success, commit, frame bytes (Return/Abort payload or 0xFF for Fault)].
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
    advance_block();
    return rec;
  }

  CapturedRecord
  run_execute_tx(wallet_calls_t const &calls,
                 std::optional<AuthClaim> claim_override = std::nullopt)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet_);
    return run_wallet_root_tx(
        client.make_invocation<u8"execute">(UINT64_MAX, calls),
        claim_override ? *claim_override : make_pubkey_claim(preimage_));
  }

  CapturedRecord run_deploy_tx(SHA256Hash const &nonce, SHA256Hash const &hash)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet_);
    return run_wallet_root_tx(
        client.make_invocation<u8"deploy">(UINT64_MAX, nonce, hash),
        make_pubkey_claim(preimage_));
  }

  static WalletCall echo_call(size_t payload_len)
  {
    x23::Vec<uint8_t, 4294967295> payload;
    payload.resize(payload_len, 0xAB);
    auto calldata = echo_meta::make_calldata<u8"echo">(payload);
    WalletCall call{};
    call.target = kEchoAddr;
    call.entrypoint =
        entrypoint_from_method_id(echo_meta::method_id<u8"echo">());
    call.gas_limit = UINT64_MAX;
    call.calldata = calldata;
    return call;
  }

  // cxs(kWalletExceptionSelector, wallet_error): abort payload the wallet
  // emits for WalletError reverts (mirrors ContractException<WalletError,
  // 0x931d3297d37d1bdfULL>::serialize() in the generated SDK trait).
  static std::vector<uint8_t> wallet_error_abort_bytes(WalletError err)
  {
    constexpr uint64_t kWalletExceptionSelector = 0x931d3297d37d1bdfULL;
    auto buf = x23::serialize_cxs(kWalletExceptionSelector, err);
    CHECK(buf.has_value());
    return {buf->begin(), buf->end()};
  }

  // cxs(kLibraryExceptionSelector, LibraryExn::Syscall{call, err}): the abort
  // payload the sdk emits on syscall_throw() — matches the native builder's
  // deploy redeploy-ban encoding.
  static std::vector<uint8_t>
  syscall_exn_abort_bytes(SyscallNo call_number, SyscallErrorCode err_code)
  {
    LibraryExn exn(LibraryExnKind::kLibraryExnSyscall);
    exn.syscall_exn() = SyscallExn{
        .call_number = call_number,
        .err_number = static_cast<SysErr>(err_code),
    };
    auto buf = x23::serialize_cxs(kLibraryExceptionSelector, exn);
    CHECK(buf.has_value());
    return {buf->begin(), buf->end()};
  }
};

// Happy path through the real resolution path: root tx with a valid pubkey
// claim to its derived, UNDEPLOYED address executes `execute` against a
// deployed wasm echo target; expect commit success and the echoed bytes.
TEST_F(DefaultWalletFallbackTest, RootFallbackExecutesWallet)
{
  setup_wallet_and_echo();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(4));
  auto rec = run_execute_tx(calls);
  ASSERT_GE(rec.size(), 2U);
  EXPECT_EQ(rec[0], 1);
  EXPECT_EQ(rec[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
}

// Wallet code demonstrably runs with zero other fixtures: empty call list
// -> EmptyCallList revert; expect root_success (Abort, not Fault) and
// commit == ExecResultTxRootFrameReverted.
TEST_F(DefaultWalletFallbackTest, RootFallbackEmptyCallListReverts)
{
  setup_wallet_and_echo();
  make_ctx();
  auto rec = run_execute_tx(wallet_calls_t{});
  auto expected = wallet_error_abort_bytes(WalletError::EmptyCallList);
  ASSERT_EQ(rec.size(), 2U + expected.size());
  EXPECT_EQ(rec[0], 1); // Abort is root_success
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), rec.begin() + 2));
}

// Separate fixture for the absent-record test: uses kMockInMemory so that
// the wallet contract record can be erased from the in-memory store after
// genesis writes it. Genesis must run (cost model required), but the wallet
// record is removed before any test runs so resolve_contract(wallet_hash)
// returns nullptr, exercising the nullptr-return path of
// make_root_fallback_wallet_runtime.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class DefaultWalletFallbackNoRecordTest : public ::testing::Test,
                                          public NativeContractTestFramework {
public:
  DefaultWalletFallbackNoRecordTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kMockInMemory)
  {
    // Genesis wrote the wallet contract record at wallet_hash. Erase it now,
    // before any transaction runs (so the StorageSnapshotCache has not yet
    // cached the entry). Subsequent lookups will see it as absent.
    erase_contract_from_mock_db(default_wallet_deployed_contract_hash().hash);
  }

protected:
  static constexpr int32_t kNativeWalletId = std::to_underlying(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet);

  PubkeyAccountPreimage preimage_{};
  ContractAddress wallet_{};
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

  static AuthClaim make_pubkey_claim(PubkeyAccountPreimage const &preimage)
  {
    AuthClaim claim{};
    claim.claim_type(AuthClaimType::kPubkeySignature);
    claim.pubkey_preimage() = preimage;
    return claim;
  }

  void make_ctx()
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(kNativeWalletId, make_native_default_wallet_contract());
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto wasm_ctx = make_hog_exec_wasm_context(RuntimeEngine::kWasmi);
    ctx_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });
  }

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
    advance_block();
    return rec;
  }

  CapturedRecord
  run_execute_tx(wallet_calls_t const &calls,
                 std::optional<AuthClaim> claim_override = std::nullopt)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet_);
    return run_wallet_root_tx(
        client.make_invocation<u8"execute">(UINT64_MAX, calls),
        claim_override ? *claim_override : make_pubkey_claim(preimage_));
  }
};

// Record absent (erased from mock DB after genesis): root invoke of the
// missing address must return nullptr from make_root_fallback_wallet_runtime
// and produce ExecResultTxDeterministicError, preserving pre-fallback
// behavior for nodes whose store does not carry the genesis wallet record.
TEST_F(DefaultWalletFallbackNoRecordTest, AbsentRecordKeepsNullResolution)
{
  preimage_ = make_preimage(1);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  wallet_ = pubkey_auth_address(preimage_).value();
  make_ctx();
  auto rec = run_execute_tx(wallet_calls_t{});
  ASSERT_GE(rec.size(), 2U);
  EXPECT_EQ(rec[0], 0);
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxDeterministicError));
}

// Auth claim from a different preimage -> InvalidAuthClaim revert.
TEST_F(DefaultWalletFallbackTest, MismatchedClaimRevertsInvalidAuthClaim)
{
  setup_wallet_and_echo();
  make_ctx();
  wallet_calls_t calls;
  calls.push_back(echo_call(4));
  auto rec = run_execute_tx(calls, make_pubkey_claim(make_preimage(99)));
  auto expected = wallet_error_abort_bytes(WalletError::InvalidAuthClaim);
  ASSERT_EQ(rec.size(), 2U + expected.size());
  EXPECT_EQ(rec[0], 1); // Abort is root_success
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), rec.begin() + 2));
}

// Unknown method id at an undeployed root address: fallback resolves the
// wallet, find_function misses -> deterministic error, frame Fault; no crash.
TEST_F(DefaultWalletFallbackTest, UnknownEntrypointFaults)
{
  setup_wallet_and_echo();
  make_ctx();
  Invocation inv{
      .invoked_address = wallet_,
      .entrypoint =
          entrypoint_from_method_id(static_cast<Runtime::MethodID>(999)),
      .gas_limit = UINT64_MAX,
      .calldata = {}};
  auto rec = run_wallet_root_tx(std::move(inv), make_pubkey_claim(preimage_));
  ASSERT_GE(rec.size(), 2U);
  EXPECT_EQ(rec[0], 0);
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxDeterministicError));
}

// Redeploy ban end-to-end: root invoke wallet `deploy` with
// contract_hash = default_wallet_deployed_contract_hash().hash ->
// kDeployContract returns InvalidSyscallInput -> wallet aborts with
// LibraryExn::Syscall{kDeployContract, InvalidSyscallInput}.
TEST_F(DefaultWalletFallbackTest, WalletHashNotDeployable)
{
  setup_wallet_and_echo();
  make_ctx();
  SHA256Hash nonce{};
  nonce.fill(1);
  auto rec =
      run_deploy_tx(nonce, default_wallet_deployed_contract_hash().hash);
  auto expected = syscall_exn_abort_bytes(
      SyscallNo::kDeployContract,
      SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
  ASSERT_EQ(rec.size(), 2U + expected.size());
  EXPECT_EQ(rec[0], 1); // Abort is root_success
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), rec.begin() + 2));
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
