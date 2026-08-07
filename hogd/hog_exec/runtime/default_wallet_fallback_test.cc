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
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
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

    write_contract(make_native_contract(kNativeWalletId));
    setup_grant_storage_token(wallet_, 1'000'000'000);

    auto echo_obj = echo_meta::load_wasm();
    ASSERT_TRUE(echo_obj);
    write_contract(echo_obj);
    write_deployment(kEchoAddr, echo_obj);
    setup_grant_storage_token(kEchoAddr, 1'000'000'000);
  }

  // [root_success, commit, frame bytes (Return/Abort payload or 0xFF for
  // Fault)].
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
    return run_wallet_root_tx(client.make_invocation<u8"execute">(UINT64_MAX,
                                                                  calls),
                              claim_override ? *claim_override
                                             : make_pubkey_claim(preimage_));
  }

  CapturedRecord run_deploy_tx(SHA256Hash const &nonce, SHA256Hash const &hash)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(wallet_);
    return run_wallet_root_tx(client.make_invocation<u8"deploy">(UINT64_MAX,
                                                                 nonce, hash),
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
  static std::vector<uint8_t> syscall_exn_abort_bytes(SyscallNo call_number,
                                                      SyscallErrorCode err_code)
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

  // Echo returns CXS of Vec<u8>(payload) — same encoding as make_calldata.
  x23::Vec<uint8_t, 4294967295> payload;
  payload.resize(4, 0xAB);
  auto echo_inner = echo_meta::make_calldata<u8"echo">(payload);
  // Wallet return: CXS of Vec<Vec<u8>>{ echo_inner }.
  //   outer count = 1 -> 1 byte (0x01, ≤ 62)
  //   inner len   = echo_inner.size() -> 1 byte (5 ≤ 62)
  //   data        = echo_inner bytes
  std::vector<uint8_t> expected;
  expected.push_back(0x01); // outer Vec length = 1
  expected.push_back(static_cast<uint8_t>(echo_inner.size())); // inner len
  expected.insert(expected.end(), echo_inner.begin(), echo_inner.end());

  ASSERT_EQ(rec.size(), 2U + expected.size());
  EXPECT_EQ(rec[0], 1);
  EXPECT_EQ(rec[1], static_cast<uint8_t>(ExecResult::ExecResultOK));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), rec.begin() + 2));
}

// Deploy success through the root-fallback path: root tx with a valid pubkey
// claim to an UNDEPLOYED address invokes `deploy` with a contract hash that IS
// in the store (echo wasm, written by setup). Assert root_success (wallet ran
// and returned a ContractAddress without faulting or aborting) and that the
// returned address matches static_get_deploy_address.
//
// Note: the kHogDb test fixture's write_address_balance path does not register
// the pre-written balance as a lockable 2PC entry, so the balance-deduction
// step in the host deploy syscall triggers a non-deterministic conflict at
// commit time. This is the same limitation that affects DefaultWalletParityTest
// (which also does not assert ExecResultOK for deploy). What matters here is
// that the fallback resolution runs the wallet's deploy entrypoint end-to-end
// and produces the correct deterministic output.
TEST_F(DefaultWalletFallbackTest, RootFallbackDeploysContract)
{
  setup_wallet_and_echo();
  make_ctx();
  // Echo wasm is already in the store from setup_wallet_and_echo().
  auto echo_obj = echo_meta::load_wasm();
  ASSERT_TRUE(echo_obj);
  SHA256Hash target_hash = sha256(*echo_obj);
  SHA256Hash nonce{};
  nonce.fill(9);
  auto rec = run_deploy_tx(nonce, target_hash);
  auto expected_addr =
      SystemCallHandler::static_get_deploy_address(wallet_, nonce, target_hash);
  // Deploy return: 32 raw bytes of ContractAddress (fixed array, no CXS
  // prefix). root_success == 1 means the wallet ran and returned an address
  // (neither fault nor abort); the address bytes must match the deterministic
  // computation regardless of the commit outcome.
  ASSERT_EQ(rec.size(), 2U + 32U);
  EXPECT_EQ(rec[0], 1); // root_success: wallet ran, no fault/abort
  EXPECT_TRUE(
      std::equal(expected_addr.begin(), expected_addr.end(), rec.begin() + 2));
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
    erase_contract_from_mock_db(
        make_deployed_contract_hash(make_native_contract(kNativeWalletId))
            .hash);
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
    return run_wallet_root_tx(client.make_invocation<u8"execute">(UINT64_MAX,
                                                                  calls),
                              claim_override ? *claim_override
                                             : make_pubkey_claim(preimage_));
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

// Tx-attached wallet record must NOT activate the fallback on a store without
// the genesis write. A SignedTransaction with contracts_to_deploy containing
// Contract{kNative, kNativeContractDefaultWallet} populates
// newly_created_contracts_, which resolve_contract searches first. Before the
// fix, resolve_contract would find the record there and enable the fallback.
// After the fix, make_root_fallback_wallet_runtime reads only the snapshot
// (snapshot_cache_.try_get_contract_by_hash) and gets nullptr because the
// record was erased from the mock DB — same outcome as
// AbsentRecordKeepsNullResolution.
TEST_F(DefaultWalletFallbackNoRecordTest, TxAttachedRecordNoFallback)
{
  preimage_ = make_preimage(1);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  wallet_ = pubkey_auth_address(preimage_).value();
  make_ctx();

  // Build a SignedTransaction that attaches the wallet's native contract
  // record.
  ContractTraitClient<default_wallet::DefaultWallet> client(wallet_);
  SignedTransaction stx;
  stx.tx.invocation =
      client.make_invocation<u8"execute">(UINT64_MAX, wallet_calls_t{});
  stx.tx.auth_claim = make_pubkey_claim(preimage_);
  stx.tx.uniquifier = make_new_uniquifier();
  stx.tx.contracts_to_deploy.push_back(*make_native_contract(kNativeWalletId));

  auto handler = make_new_system_call_handler(stx, *ctx_);
  auto res = handler.root_invoke();
  CapturedRecord rec;
  rec.push_back(res.is_root_success() ? 1 : 0);
  ExecResult commit = handler.try_commit_transaction();
  rec.push_back(static_cast<uint8_t>(commit));
  advance_block();

  // Snapshot has no wallet record (erased after genesis) → nullptr from
  // snapshot cache → no fallback → DeterministicError, same as
  // AbsentRecordKeepsNullResolution.
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
// rec[2] == 0xFF pins the Fault branch: null resolution would yield size==2
// (no buffer appended), so the exact-size check discriminates the two paths.
TEST_F(DefaultWalletFallbackTest, UnknownEntrypointFaults)
{
  setup_wallet_and_echo();
  make_ctx();
  Invocation inv{.invoked_address = wallet_,
                 .entrypoint = entrypoint_from_method_id(
                     static_cast<Runtime::MethodID>(999)),
                 .gas_limit = UINT64_MAX,
                 .calldata = {}};
  auto rec = run_wallet_root_tx(std::move(inv), make_pubkey_claim(preimage_));
  ASSERT_EQ(rec.size(), 3U);
  EXPECT_EQ(rec[0], 0);
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxDeterministicError));
  EXPECT_EQ(rec[2], 0xFF);
}

// Root-only guard regression pin: the fallback MUST NOT fire for nested
// invocations. A WalletCall targeting a second undeployed pubkey-derived
// address is dispatched via kInvoke; at that point invocation_frame_stack_ is
// non-empty, so resolve_runtime returns nullptr and kInvoke is rejected
// pre-frame. The wallet's execute handler maps that pre-frame rejection to
// WalletError::CallFaulted (precompile_default_wallet.cc:182-185).
//
// Discrimination: if the invocation_frame_stack_.empty() guard were removed,
// the nested kInvoke would reach the fallback, find the genesis wallet record
// ({kNative, 2}), but fail to locate id 2 in the test-context NativeContext
// (which only registers kNativeContractDefaultWallet with TEST id 60 in the
// parity suite). That code-path produces a different error. More importantly,
// the test asserts the tx REVERTS via the wallet (CallFaulted abort) rather
// than the nested address somehow resolving to a runnable frame, which would
// violate the root-only invariant and could CHECK-crash under deeper nesting.
TEST_F(DefaultWalletFallbackTest, NestedInvokeUndeployedNoFallback)
{
  setup_wallet_and_echo();
  make_ctx();
  // Second undeployed pubkey-derived address (different preimage, no
  // deployment row written, no storage grant needed since kInvoke is rejected
  // before any frame runs).
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  ContractAddress undeployed2 = pubkey_auth_address(make_preimage(2)).value();
  WalletCall nested_call{};
  nested_call.target = undeployed2;
  nested_call.entrypoint =
      entrypoint_from_method_id(0); // irrelevant: pre-frame rejection
  nested_call.gas_limit = UINT64_MAX;
  wallet_calls_t calls;
  calls.push_back(nested_call);
  auto rec = run_execute_tx(calls);
  // kInvoke pre-frame rejection → CallFaulted abort (no fallback fires nested).
  auto expected = wallet_error_abort_bytes(WalletError::CallFaulted);
  ASSERT_EQ(rec.size(), 2U + expected.size());
  EXPECT_EQ(rec[0], 1); // Abort is root_success
  EXPECT_EQ(rec[1],
            static_cast<uint8_t>(ExecResult::ExecResultTxRootFrameReverted));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), rec.begin() + 2));
}

// Redeploy ban end-to-end: root invoke wallet `deploy` with the wallet's own
// contract hash -> kDeployContract returns InvalidSyscallInput -> wallet aborts
// with LibraryExn::Syscall{kDeployContract, InvalidSyscallInput}.
TEST_F(DefaultWalletFallbackTest, WalletHashNotDeployable)
{
  setup_wallet_and_echo();
  make_ctx();
  SHA256Hash nonce{};
  nonce.fill(1);
  auto rec = run_deploy_tx(
      nonce,
      make_deployed_contract_hash(make_native_contract(kNativeWalletId)).hash);
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
