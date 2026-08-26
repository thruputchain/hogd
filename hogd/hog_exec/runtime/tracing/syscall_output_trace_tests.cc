#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hog_exec_constants.hh"

#include "auth_claim.hh"
#include "auth_token.hh"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/ffi_types.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

using SyscallTraceVec = x23::Vec<SyscallTrace, 0xffffffff>;

HostFunctionStatusXdr
ok_status()
{
  HostFunctionStatusXdr status(HostFnErrorCode::NoneOrRecoverable);
  status.syscall_return_code() = SyscallErrorCode::kSyscallErrorCodeNone;
  return status;
}

HostFunctionStatusXdr
error_status(SyscallErrorCode code)
{
  HostFunctionStatusXdr status(HostFnErrorCode::NoneOrRecoverable);
  status.syscall_return_code() = code;
  return status;
}

SyscallTrace
make_entry(GasAmount gas_at_start, GasAmount gas_at_end, SyscallTraceBody body,
           HostFunctionStatusXdr status)
{
  return SyscallTrace{
      .gas_remaining_at_start = gas_at_start,
      .gas_remaining_at_end = gas_at_end,
      .body = std::move(body),
      .status = std::move(status),
  };
}

// Test-only stub accepting any rv32-tagged blob, so DeployContract can run
// against an opaque attached contract (same rationale as
// selective_deployment_test).
class NoopRv32Context : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const & /*unused*/) const override
  {
    return InvokeErrorCode::None;
  }

  x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const & /*unused*/,
                   SystemCallHandler * /*unused*/) override
  {
    co_return nullptr;
  }
};

} // namespace

// Content pins for the syscall trace outputs that had no assertions anywhere:
// GetBlockHeight, GetTransactionStatistics(GasRemaining), GetMsgSender,
// NonnegativeIntGet (conditional current_set presence), SHA256Update/Finalize,
// GetAuthClaim, Mint/ConsumeAuthToken (full payload recorded even when the
// caller's buffer truncates), RCQ Insert/Consume/PeekLen (full entry
// recorded), and GetDeployAddress/DeployContract. Native contracts drive the
// syscalls; the trace recording is the same wrap_after_parse path production
// rv32 uses, with only the guest-side instruction gas absent, so the gas
// literals here are the schedule's base costs alone. Regenerate them
// deliberately on a schedule change (the failure message prints the observed
// trace).
// NOLINTNEXTLINE(misc-use-internal-linkage)
class SyscallOutputTraceTest : public ::testing::Test,
                               public NativeContractTestFramework {
public:
  SyscallOutputTraceTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  constexpr static GasAmount kGasLimit = 100'000;
  constexpr static ContractAddress kAddr = make_contract_address(0);

  std::unique_ptr<NativeContext>
  make_single_contract_context(NativeContract::function_t fn)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, std::move(fn));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
    auto context = std::make_unique<NativeContext>(std::move(cmap));

    auto contract = make_native_contract(0);
    write_contract(contract);
    write_deployment(kAddr, contract);
    setup_grant_storage_token(kAddr, 1'000'000'000);
    return context;
  }

  SignedTransaction make_tx()
  {
    return SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(0),
                                         .gas_limit = kGasLimit,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
  }
};

TEST_F(SyscallOutputTraceTest, BlockHeightStatsAndMsgSenderTracesAreExact)
{
  const auto executing_height = static_cast<BlockHeight>(get_block_number());

  auto context = make_single_contract_context(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        BlockHeight height{};
        auto h = co_await ctx.syscall<SyscallNo::kGetBlockHeight>(height);
        if (!h.has_value() ||
            h.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint64_t gas_out = 0;
        auto s = co_await ctx.syscall<SyscallNo::kGetTransactionStatistics>(
            TransactionStatistic::GasRemaining, gas_out);
        if (!s.has_value() ||
            s.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        ContractAddress sender{};
        auto m = co_await ctx.syscall<SyscallNo::kGetMsgSender>(sender);
        if (!m.has_value() ||
            m.value() != SyscallErrorCode::kSyscallErrorCodeIsRootTransaction) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto handler = make_new_system_call_handler(make_tx(), *context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kGetBlockHeight);
    body.get_block_height().outputs =
        SyscallGetBlockHeightOutputs{.result = executing_height};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kGetTransactionStatistics);
    body.get_transaction_statistics().inputs.statistic =
        TransactionStatistic::GasRemaining;
    body.get_transaction_statistics().outputs =
        SyscallGetTransactionStatisticsOutputs{.result = 100'000};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kGetMsgSender);
    expected.push_back(make_entry(
        100'000, 100'000, std::move(body),
        error_status(SyscallErrorCode::kSyscallErrorCodeIsRootTransaction)));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallOutputTraceTest, NnintGetTraceConditionalPresenceIsExact)
{
  const StorageKeyContractData set_key = make_storage_key("trace_nnint_set");
  const StorageKeyContractData unset_key =
      make_storage_key("trace_nnint_unset");

  auto context = make_single_contract_context(
      [set_key,
       unset_key](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const int128_t set_value = 5;
        auto w = co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
            set_key, &set_value, int128_t{7},
            std::to_underlying(StorageDeltaFlags::None));
        if (!w.has_value() ||
            w.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        int128_t current_set = 0;
        bool current_set_opt = false;
        int128_t current_add = 0;
        auto g1 = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
            set_key, false, current_set, current_set_opt, current_add);
        if (!g1.has_value() ||
            g1.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            !current_set_opt || current_set != 5 || current_add != 7) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto g2 = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
            unset_key, false, current_set, current_set_opt, current_add);
        if (!g2.has_value() ||
            g2.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            current_set_opt || current_add != 0) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto handler = make_new_system_call_handler(make_tx(), *context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kNonnegativeIntSetAdd);
    body.nonnegative_int_set_add().inputs.key = set_key;
    body.nonnegative_int_set_add().inputs.set_value = to_xdr(int128_t{5});
    body.nonnegative_int_set_add().inputs.add_value = to_xdr(int128_t{7});
    body.nonnegative_int_set_add().inputs.flags = 0;
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kNonnegativeIntGet);
    body.nonnegative_int_get().inputs.key = set_key;
    body.nonnegative_int_get().inputs.force_load = false;
    SyscallNonnegativeIntGetOutputs outputs;
    outputs.current_set = to_xdr(int128_t{5});
    outputs.current_add = to_xdr(int128_t{7});
    body.nonnegative_int_get().outputs = std::move(outputs);
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    // The unset key's trace output must OMIT current_set entirely -- the
    // conditional-presence rule, not a zero value.
    SyscallTraceBody body(SyscallNo::kNonnegativeIntGet);
    body.nonnegative_int_get().inputs.key = unset_key;
    body.nonnegative_int_get().inputs.force_load = false;
    SyscallNonnegativeIntGetOutputs outputs;
    outputs.current_add = to_xdr(int128_t{0});
    body.nonnegative_int_get().outputs = std::move(outputs);
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallOutputTraceTest, Sha256IncrementalFinalizeTraceIsExact)
{
  const std::array<uint8_t, 3> part1 = {'a', 'b', 'c'};
  const std::array<uint8_t, 3> part2 = {'d', 'e', 'f'};
  const std::array<uint8_t, 6> whole = {'a', 'b', 'c', 'd', 'e', 'f'};
  const SHA256Hash expected_hash = sha256(std::span(whole));

  auto context = make_single_contract_context(
      [part1, part2, expected_hash](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto u1 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0U, std::as_bytes(std::span(part1)), true);
        if (!u1.has_value() ||
            u1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto u2 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0U, std::as_bytes(std::span(part2)), false);
        if (!u2.has_value() ||
            u2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        SHA256Hash out{};
        auto f = co_await ctx.syscall<SyscallNo::kSHA256Finalize>(0U, out);
        if (!f.has_value() ||
            f.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            out != expected_hash) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto handler = make_new_system_call_handler(make_tx(), *context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kSHA256Update);
    body.sha256_update().inputs.index = 0;
    body.sha256_update().inputs.buffer.append_range(part1);
    body.sha256_update().inputs.reset = true;
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kSHA256Update);
    body.sha256_update().inputs.index = 0;
    body.sha256_update().inputs.buffer.append_range(part2);
    body.sha256_update().inputs.reset = false;
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kSHA256Finalize);
    body.sha256_finalize().inputs.index = 0;
    body.sha256_finalize().outputs =
        SyscallSHA256FinalizeOutputs{.hash = expected_hash};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Pins every contract-observable behavior of the incremental-hash buffers in
// one traced sequence: interleaved independent indexes, the append-overflow
// error leaving the buffer intact, finalize consuming its buffer, index reuse
// via reset, and the finalize byte counts feeding
// sha256_total_bytes_processed. The exact trace (inputs, digests, error
// statuses, gas) must not move when the buffer representation changes.
TEST_F(SyscallOutputTraceTest, Sha256IncrementalLifecycleTraceIsExact)
{
  const std::array<uint8_t, 3> abc = {'a', 'b', 'c'};
  const std::array<uint8_t, 4> wxyz = {'w', 'x', 'y', 'z'};
  const std::array<uint8_t, 3> def = {'d', 'e', 'f'};
  const std::array<uint8_t, 5> fresh = {'f', 'r', 'e', 's', 'h'};
  const std::vector<uint8_t> oversized(kSHA256BufferMaxSize, 0xEE);

  const SHA256Hash hash_wxyz = sha256(std::span(wxyz));
  const SHA256Hash hash_abcdef =
      sha256(std::vector<uint8_t>{'a', 'b', 'c', 'd', 'e', 'f'});
  const SHA256Hash hash_fresh = sha256(std::span(fresh));

  auto context = make_single_contract_context(
      [abc, wxyz, def, fresh, oversized, hash_wxyz, hash_abcdef,
       hash_fresh](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto expect = [](auto r, SyscallErrorCode code) {
          return r.has_value() && r.value() == code;
        };
        constexpr auto kOk = SyscallErrorCode::kSyscallErrorCodeNone;
        constexpr auto kInvalid =
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput;

        if (!expect(ctx.syscall<SyscallNo::kSHA256Update>(
                        0U, std::as_bytes(std::span(abc)), true),
                    kOk)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (!expect(ctx.syscall<SyscallNo::kSHA256Update>(
                        1U, std::as_bytes(std::span(wxyz)), true),
                    kOk)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Overflowing append: rejected, and index 0 keeps hashing as if the
        // call never happened.
        if (!expect(ctx.syscall<SyscallNo::kSHA256Update>(
                        0U, std::as_bytes(std::span(oversized)), false),
                    kInvalid)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (!expect(ctx.syscall<SyscallNo::kSHA256Update>(
                        0U, std::as_bytes(std::span(def)), false),
                    kOk)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash out{};
        if (!expect(ctx.syscall<SyscallNo::kSHA256Finalize>(1U, out), kOk) ||
            out != hash_wxyz) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (!expect(ctx.syscall<SyscallNo::kSHA256Finalize>(0U, out), kOk) ||
            out != hash_abcdef) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Finalize consumed index 0.
        if (!expect(ctx.syscall<SyscallNo::kSHA256Finalize>(0U, out),
                    kInvalid)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        // The index is reusable with a fresh reset.
        if (!expect(ctx.syscall<SyscallNo::kSHA256Update>(
                        0U, std::as_bytes(std::span(fresh)), true),
                    kOk)) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (!expect(ctx.syscall<SyscallNo::kSHA256Finalize>(0U, out), kOk) ||
            out != hash_fresh) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto handler = make_new_system_call_handler(make_tx(), *context);
  handler.enable_tracing();
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  auto update_entry = [](uint32_t index, std::span<const uint8_t> buffer,
                         bool reset, HostFunctionStatusXdr status) {
    SyscallTraceBody body(SyscallNo::kSHA256Update);
    body.sha256_update().inputs.index = index;
    body.sha256_update().inputs.buffer.append_range(buffer);
    body.sha256_update().inputs.reset = reset;
    return make_entry(100'000, 100'000, std::move(body), std::move(status));
  };
  auto finalize_entry = [](uint32_t index, std::optional<SHA256Hash> hash,
                           HostFunctionStatusXdr status) {
    SyscallTraceBody body(SyscallNo::kSHA256Finalize);
    body.sha256_finalize().inputs.index = index;
    if (hash.has_value()) {
      body.sha256_finalize().outputs =
          SyscallSHA256FinalizeOutputs{.hash = hash.value()};
    }
    return make_entry(100'000, 100'000, std::move(body), std::move(status));
  };
  const auto invalid =
      error_status(SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  SyscallTraceVec expected;
  expected.push_back(update_entry(0, abc, true, ok_status()));
  expected.push_back(update_entry(1, wxyz, true, ok_status()));
  expected.push_back(update_entry(0, oversized, false, invalid));
  expected.push_back(update_entry(0, def, false, ok_status()));
  expected.push_back(finalize_entry(1, hash_wxyz, ok_status()));
  expected.push_back(finalize_entry(0, hash_abcdef, ok_status()));
  expected.push_back(finalize_entry(0, std::nullopt, invalid));
  expected.push_back(update_entry(0, fresh, true, ok_status()));
  expected.push_back(finalize_entry(0, hash_fresh, ok_status()));

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  // Byte counts flow through finalize into the transaction stats: 4 (wxyz) +
  // 6 (abcdef) + 5 (fresh); the failed append and failed finalize add nothing.
  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed, 15U);

  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallOutputTraceTest, AuthClaimAndAuthTokenTracesAreExact)
{
  const auto executing_height = static_cast<BlockHeight>(get_block_number());

  AuthClaim claim{};
  claim.source_address_claim().allowed_keys = BitMap{0b1011};
  claim.source_address_claim().required_weight = Weight{7};

  const std::array<uint8_t, 4> payload = {1, 2, 3, 4};
  AuthTokenNode node;
  node.consumer_address = kAddr;
  node.payload.append_range(payload);
  auto serialized_node = x23::serialize(node);

  auto context = make_single_contract_context(
      [payload, serialized_node,
       claim](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        generated::AuthClaim claim_out{};
        auto c = co_await ctx.syscall<SyscallNo::kGetAuthClaim>(claim_out);
        if (!c.has_value() ||
            c.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint32_t token_id = 0;
        auto m = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{(co_await ctx.handler().get_block_height()).value() +
                        5},
            std::span<const std::byte>(reinterpret_cast<const std::byte *>(
                                           serialized_node.data()),
                                       serialized_node.size()),
            token_id);
        if (!m.has_value() ||
            m.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // A 2-byte buffer for a 4-byte payload: written_len reports the FULL
        // length, the buffer receives the prefix, and the trace must record
        // the whole payload.
        std::array<std::byte, 2> small_buffer{};
        ContractAddress minter{};
        RuntimeMemoryLength written_len = 0;
        uint32_t unwrapped_start = 0;
        uint32_t unwrapped_count = 0;
        auto consume = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>(small_buffer), minter, written_len,
            unwrapped_start, unwrapped_count);
        if (!consume.has_value() ||
            consume.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (written_len != payload.size() ||
            small_buffer[0] != std::byte{payload[0]} ||
            small_buffer[1] != std::byte{payload[1]} || unwrapped_count != 0) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto tx = make_tx();
  tx.tx.auth_claim = claim;

  auto handler = make_new_system_call_handler(tx, *context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kGetAuthClaim);
    body.get_auth_claim().outputs =
        SyscallGetAuthClaimOutputs{.auth_claim = claim};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kMintAuthToken);
    body.mint_auth_token().inputs.expiry_height = executing_height + 5;
    body.mint_auth_token().inputs.serialized_auth_token.append_range(
        serialized_node);
    body.mint_auth_token().outputs = SyscallMintAuthTokenOutputs{.token_id = 0};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kConsumeAuthToken);
    body.consume_auth_token().inputs.token_id = 0;
    SyscallConsumeAuthTokenOutputs outputs;
    outputs.payload.append_range(payload);
    outputs.token_minter = kAddr;
    outputs.auth_payload_written_len = static_cast<uint32_t>(payload.size());
    outputs.unwrapped_token_id_start = 0;
    outputs.unwrapped_token_id_count = 0;
    body.consume_auth_token().outputs = std::move(outputs);
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallOutputTraceTest, RcqTracesRecordFullEntryAndPeekLen)
{
  const StorageKeyContractData key = make_storage_key("trace_rcq_key");
  const std::array<uint8_t, 3> entry_bytes = {0xAA, 0xBB, 0xCC};

  auto context = make_single_contract_context(
      [key, entry_bytes](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto ins = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
            key, std::as_bytes(std::span(entry_bytes)));
        if (!ins.has_value() ||
            ins.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        RuntimeMemoryLength len = 0;
        auto peek =
            co_await ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(key, len);
        if (!peek.has_value() ||
            peek.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            len != entry_bytes.size()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // A 1-byte buffer for a 3-byte entry: written_len reports the full
        // length, the buffer gets the prefix, and the trace must record the
        // whole entry.
        std::array<std::byte, 1> small_buffer{};
        RuntimeMemoryLength written_len = 0;
        auto consume =
            co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                key, false, std::span<std::byte>(small_buffer), written_len);
        if (!consume.has_value() ||
            consume.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (written_len != entry_bytes.size() ||
            small_buffer[0] != std::byte{entry_bytes[0]}) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto handler = make_new_system_call_handler(make_tx(), *context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  QueueEntry full_entry;
  full_entry.append_range(entry_bytes);

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kRetrocausalQueueInsert);
    body.retrocausal_queue_insert().inputs.key = key;
    body.retrocausal_queue_insert().inputs.queue_entry = full_entry;
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kRetrocausalQueuePeekLen);
    body.retrocausal_queue_peek_len().inputs.key = key;
    body.retrocausal_queue_peek_len().outputs =
        SyscallRetrocausalQueuePeekLenOutputs{
            .len = static_cast<uint32_t>(entry_bytes.size())};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kRetrocausalQueueConsume);
    body.retrocausal_queue_consume().inputs.key = key;
    body.retrocausal_queue_consume().inputs.peek = false;
    body.retrocausal_queue_consume().outputs =
        SyscallRetrocausalQueueConsumeOutputs{.queue_entry = full_entry};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallOutputTraceTest, DeployAddressTracesAreExact)
{
  Contract blob(ContractType::kRv32);
  blob.rv32_contract().data.resize_uninitialized(32);
  for (size_t i = 0; i < blob.rv32_contract().data.size(); ++i) {
    blob.rv32_contract().data[i] = static_cast<uint8_t>((i * 13) + 3);
  }
  auto blob_hash = sha256(blob);

  constexpr static SHA256Hash kNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 77;
    return nonce;
  }();

  const auto expected_addr =
      SystemCallHandler::static_get_deploy_address(kAddr, kNonce, blob_hash);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [blob_hash, expected_addr](
                  NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                ContractAddress predicted{};
                auto g = co_await ctx.syscall<SyscallNo::kGetDeployAddress>(
                    static_cast<const ContractAddress *>(nullptr), kNonce,
                    blob_hash, predicted);
                if (!g.has_value() ||
                    g.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
                    predicted != expected_addr) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }

                ContractAddress deployed{};
                auto d =
                    co_await ctx.syscall<SyscallNo::kDeployContract>(kNonce,
                                                                     blob_hash,
                                                                     deployed);
                if (!d.has_value() ||
                    d.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
                    deployed != expected_addr) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                co_return InvokeStatus<void>{};
              });
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
  NativeContext native_context(std::move(cmap));
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(kAddr, contract);
  setup_grant_storage_token(kAddr, 1'000'000'000);
  setup_grant_storage_token(expected_addr, 1'000'000'000);

  auto tx = make_tx();
  tx.tx.contracts_to_deploy = {blob};

  auto handler = make_new_system_call_handler(tx, context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kGetDeployAddress);
    SyscallGetDeployAddressInputs inputs;
    inputs.nonce = kNonce;
    inputs.contract_hash = blob_hash;
    body.get_deploy_address().inputs = std::move(inputs);
    body.get_deploy_address().outputs =
        SyscallGetDeployAddressOutputs{.to_be_deployed = expected_addr};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kDeployContract);
    body.deploy_contract().inputs.nonce = kNonce;
    body.deploy_contract().inputs.contract_hash = blob_hash;
    body.deploy_contract().outputs =
        SyscallDeployContractOutputs{.deployed_address = expected_addr};
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// BytestringGet follows the same truncation convention as ConsumeAuthToken
// and RetrocausalQueueConsume: the trace records the FULL stored value while
// the caller's undersized buffer receives the prefix and written_len reports
// the full length.
TEST_F(SyscallOutputTraceTest, BytestringGetTraceRecordsFullValueWhenTruncated)
{
  const StorageKeyContractData key = make_storage_key("trace_bs_key");
  const std::array<uint8_t, 8> value = {'1', '2', '3', '4', '5', '6', '7', '8'};

  auto context = make_single_contract_context(
      [key,
       value](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto w = co_await ctx.syscall<SyscallNo::kBytestringSet>(
            key, std::as_bytes(std::span(value)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!w.has_value() ||
            w.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        std::array<std::byte, 4> small_buffer{};
        RuntimeMemoryLength written_len = 0;
        auto g = co_await ctx.syscall<SyscallNo::kBytestringGet>(
            key, std::span<std::byte>(small_buffer), written_len);
        if (!g.has_value() ||
            g.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (written_len != value.size() ||
            small_buffer[0] != std::byte{value[0]} ||
            small_buffer[3] != std::byte{value[3]}) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto handler = make_new_system_call_handler(make_tx(), *context);
  handler.enable_tracing();
  ASSERT_TRUE(handler.root_invoke()().is_root_success());

  SyscallTraceVec expected;
  {
    SyscallTraceBody body(SyscallNo::kBytestringSet);
    body.bytestring_set().inputs.key = key;
    body.bytestring_set().inputs.bytestring.append_range(value);
    body.bytestring_set().inputs.flags = 0;
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }
  {
    SyscallTraceBody body(SyscallNo::kBytestringGet);
    body.bytestring_get().inputs.key = key;
    SyscallBytestringGetOutputs outputs;
    outputs.bytestring.append_range(value);
    body.bytestring_get().outputs = std::move(outputs);
    expected.push_back(
        make_entry(100'000, 100'000, std::move(body), ok_status()));
  }

  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls, expected)
      << x23::show(handler.get_trace()->system_calls);

  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
