#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Exercises SyscallNo::kRetrocausalQueuePeekLen: after inserting a queue entry,
// peek-len must report the same byte length as that entry (without consuming).
class SyscallRcqPeekTest : public ::testing::Test,
                           public NativeContractTestFramework {
public:
  SyscallRcqPeekTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  static constexpr StorageKeyContractData kStorageKey =
      NativeContractTestFramework::make_storage_key("rcq_peek");
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(SyscallRcqPeekTest, PeekLenMatchesInsertedEntry)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Serialized uint32_t payload is 4 bytes; calldata carries the expected len.
  uint32_t expected_len = sizeof(uint32_t);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &ctx,
         uint32_t expected_len) -> InvokeStatus<void> {
        uint32_t payload = 0xC0FFEEU;
        auto insert_status = ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
            SyscallRcqPeekTest::kStorageKey,
            std::as_bytes(std::span(&payload, 1)));
        if (!insert_status.has_value() ||
            insert_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        RuntimeMemoryLength len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
            SyscallRcqPeekTest::kStorageKey, len);
        if (!peek_status.has_value() ||
            peek_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        if (len != expected_len) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          expected_len);
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// With no retrocausal queue at the key, PeekLen returns the same errors as
// consume on an empty queue (no entry / no storage yet).
TEST_F(SyscallRcqPeekTest, PeekLenNoQueueEntry)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        RuntimeMemoryLength len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
            SyscallRcqPeekTest::kStorageKey, len);
        if (!peek_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto code = peek_status.value();
        if (code !=
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto calldata = Serializer<
      NativeContractBuilder::kDefaultSerializationFormat>::serialize();
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(1),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_F(SyscallRcqPeekTest, PeekLenNoQueueEntryEmitsNoStorageDelta)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      3, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        RuntimeMemoryLength len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
            SyscallRcqPeekTest::kStorageKey, len);
        if (!peek_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (peek_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto calldata = Serializer<
      NativeContractBuilder::kDefaultSerializationFormat>::serialize();
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(3),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  handler.enable_tracing();

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);
  EXPECT_TRUE(trace->storage_writes.empty());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_F(SyscallRcqPeekTest, PeekConsumeNoQueueEntryEmitsNoStorageDelta)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      6, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        uint32_t out = 0;
        RuntimeMemoryLength written_len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
            SyscallRcqPeekTest::kStorageKey, true,
            std::as_writable_bytes(std::span(&out, 1)), written_len);
        if (!peek_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (peek_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (written_len != 0) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto calldata = Serializer<
      NativeContractBuilder::kDefaultSerializationFormat>::serialize();
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(6),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  handler.enable_tracing();

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);
  EXPECT_TRUE(trace->storage_writes.empty());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_F(SyscallRcqPeekTest, PeekLenAfterDeleteNoQueueEntryPreservesDeleteDelta)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      7, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto delete_status = ctx.syscall<SyscallNo::kDeleteObject>(
            SyscallRcqPeekTest::kStorageKey,
            std::to_underlying(StorageDeltaFlags::None));
        if (!delete_status.has_value() ||
            delete_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        RuntimeMemoryLength len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
            SyscallRcqPeekTest::kStorageKey, len);
        if (!peek_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (peek_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (len != 0) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto calldata = Serializer<
      NativeContractBuilder::kDefaultSerializationFormat>::serialize();
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(7),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  handler.enable_tracing();

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto const *trace = handler.get_trace();
  ASSERT_NE(trace, nullptr);
  ASSERT_EQ(trace->storage_writes.size(), 1);
  EXPECT_EQ(trace->storage_writes[0].delta.type(),
            StorageDeltaType::kDeltaTypeDelete);

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_F(SyscallRcqPeekTest, PeekLenOnSnapshotQueueEmitsNoStorageDelta)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      4, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        uint32_t payload = 0xC0FFEEU;
        auto insert_status = ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
            SyscallRcqPeekTest::kStorageKey,
            std::as_bytes(std::span(&payload, 1)));
        if (!insert_status.has_value() ||
            insert_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      5,
      [](NativeSyscallContext &ctx,
         uint32_t expected_len) -> InvokeStatus<void> {
        RuntimeMemoryLength len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
            SyscallRcqPeekTest::kStorageKey, len);
        if (!peek_status.has_value() ||
            peek_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (len != expected_len) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto insert_calldata = Serializer<
      NativeContractBuilder::kDefaultSerializationFormat>::serialize();
  ASSERT_TRUE(insert_calldata.has_value());

  auto insert_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(4),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = insert_calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  ASSERT_TRUE(insert_handler.root_invoke().is_root_success());
  ASSERT_EQ(insert_handler.try_commit_transaction(), ExecResult::ExecResultOK);

  advance_block();

  auto peek_calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          static_cast<uint32_t>(sizeof(uint32_t)));
  ASSERT_TRUE(peek_calldata.has_value());

  auto peek_handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(5),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = peek_calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);
  peek_handler.enable_tracing();

  auto root_res = peek_handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto const *trace = peek_handler.get_trace();
  ASSERT_NE(trace, nullptr);
  EXPECT_TRUE(trace->storage_writes.empty());

  ASSERT_EQ(peek_handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// A bytestring at the key is not a retrocausal queue; PeekLen must not
// succeed (same idea as RCQ consume on a bytestring slot).
TEST_F(SyscallRcqPeekTest, PeekLenFailsWhenBytestringOccupiesKey)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      2, [](NativeSyscallContext &ctx, Bytestring data) -> InvokeStatus<void> {
        auto set_status = ctx.syscall<SyscallNo::kBytestringSet>(
            SyscallRcqPeekTest::kStorageKey, std::as_bytes(std::span(data)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!set_status.has_value() ||
            set_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        RuntimeMemoryLength len = 0;
        auto peek_status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
            SyscallRcqPeekTest::kStorageKey, len);
        if (!peek_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (peek_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  Bytestring written;
  written.resize(4);
  written[0] = 0xDEU;
  written[1] = 0xADU;
  written[2] = 0xBEU;
  written[3] = 0xEFU;

  auto calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          written);
  ASSERT_TRUE(calldata.has_value());

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(2),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = calldata.value()},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

namespace {
// Distinct queue key per index, so peeking them accumulates one concurrent
// observation each in the global observed_queue_entry_sequence.
StorageKeyContractData
make_obs_cap_key(uint32_t index)
{
  StorageKeyContractData key{};
  key[0] = 'o';
  key[1] = 'c';
  key[2] = 'k';
  key[4] = static_cast<uint8_t>(index);
  key[5] = static_cast<uint8_t>(index >> 8U);
  key[6] = static_cast<uint8_t>(index >> 16U);
  key[7] = static_cast<uint8_t>(index >> 24U);
  return key;
}
} // namespace

// #2894: observing kMaxConcurrentRcqReadsPerTx + 1 concurrent RCQ entries must
// return a deterministic InvalidSyscallInput at the cap rather than overflowing
// the bounded observed_queue_entry_sequence and terminating the executor. A
// contract that handles the error commits normally.
TEST_F(SyscallRcqPeekTest,
       ObservationCapReturnsSyscallErrorInsteadOfTerminating)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, int128_t{kHogDivisibility} *
                                               int128_t{kTotalHogSupply});

  constexpr uint32_t kObservationCount = kMaxConcurrentRcqReadsPerTx + 1;

  NativeContractBuilder builder;
  // method 8: insert one entry into each of `count` distinct queue keys.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      8, [](NativeSyscallContext &ctx, uint32_t count) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < count; ++i) {
          auto payload = static_cast<uint8_t>(i);
          auto status = ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
              make_obs_cap_key(i), std::as_bytes(std::span(&payload, 1)));
          if (!status.has_value() ||
              status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      });
  // method 9: peek `count` keys. The first kMaxConcurrentRcqReadsPerTx succeed;
  // the cap-plus-one peek returns InvalidSyscallInput (not a crash).
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      9, [](NativeSyscallContext &ctx, uint32_t count) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < count; ++i) {
          RuntimeMemoryLength len = 0;
          auto status = ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
              make_obs_cap_key(i), len);
          if (!status.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (i < kMaxConcurrentRcqReadsPerTx) {
            if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
                len != 1) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
          }
          else if (status.value() !=
                   SyscallErrorCode::
                       kSyscallErrorCodeRetrocausalQueueReadLimitReached) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto make_tx = [&](uint32_t method, Calldata calldata) {
    return SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = contract_addr,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = std::move(calldata)},
                          .uniquifier = make_new_uniquifier()}};
  };

  // Commit the inserts first so the peeking tx observes them as concurrent.
  auto insert_calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          kObservationCount);
  ASSERT_TRUE(insert_calldata.has_value());
  auto insert_handler =
      make_new_system_call_handler(make_tx(8, insert_calldata.value()),
                                   *context);
  ASSERT_TRUE(insert_handler.root_invoke().is_root_success());
  ASSERT_EQ(insert_handler.try_commit_transaction(), ExecResult::ExecResultOK);

  // The cap-plus-one observation returns InvalidSyscallInput; the process stays
  // alive and the contract (which handles the error) commits.
  auto peek_calldata =
      Serializer<NativeContractBuilder::kDefaultSerializationFormat>::serialize(
          kObservationCount);
  ASSERT_TRUE(peek_calldata.has_value());
  auto peek_handler =
      make_new_system_call_handler(make_tx(9, peek_calldata.value()), *context);
  ASSERT_TRUE(peek_handler.root_invoke().is_root_success());
  EXPECT_EQ(peek_handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// #3043: A destructive concurrent RCQ consume that crosses the per-tx
// observation cap must not persist the cap-crossing consume. Proposal mode
// previously appended the cap-crossing entry to the consume delta before
// load_next_queue_entry converted the read into
// kSyscallErrorCodeRetrocausalQueueReadLimitReached, while validation replay
// returned the same handleable error before advancing the cursor. The two
// executions therefore staged different RCQ bytes and produced different
// post-block DbHash values -- a consensus divergence a malicious contract can
// trigger on demand. This test drives the exact scenario and asserts proposal
// and validation commit identical state.
TEST_F(SyscallRcqPeekTest, ConsumeCapErrorAgreesProposalAndValidationDbHash)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, int128_t{kHogDivisibility} *
                                               int128_t{kTotalHogSupply});

  NativeContractBuilder builder;
  // method 10: insert two entries into target queue T, plus one entry into each
  // of kMaxConcurrentRcqReadsPerTx - 1 distinct filler queues. Committing this
  // tx first makes those entries current-block concurrent entries for the
  // consume tx below.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      10, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto target = NativeContractTestFramework::make_storage_key("target_q");
        for (uint8_t payload : {uint8_t{0xA0}, uint8_t{0xA1}}) {
          auto res = ctx.handler().retrocausal_queue_insert(
              target, std::as_bytes(std::span(&payload, 1)));
          if (!res.has_value()) {
            return res.error().to_invoke_error();
          }
          if (!res.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx - 1; ++i) {
          auto payload = static_cast<uint8_t>(i);
          auto res = ctx.handler().retrocausal_queue_insert(
              make_obs_cap_key(i), std::as_bytes(std::span(&payload, 1)));
          if (!res.has_value()) {
            return res.error().to_invoke_error();
          }
          if (!res.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      });
  // method 11: destructively consume T[0] (concurrent observation #1), peek
  // every filler to fill the observation sequence exactly to the cap, then
  // destructively consume T again. That last consume crosses the cap and must
  // return the read-limit error, which the contract handles as a stop condition
  // and returns success.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      11, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto target = NativeContractTestFramework::make_storage_key("target_q");

        auto first =
            ctx.handler().retrocausal_queue_consume(target, /*peek=*/false);
        if (!first.has_value()) {
          return first.error().to_invoke_error();
        }
        if (!first.value().has_value() || first.value().value().size() != 1) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx - 1; ++i) {
          auto peek =
              ctx.handler().retrocausal_queue_consume(make_obs_cap_key(i),
                                                      /*peek=*/true);
          if (!peek.has_value()) {
            return peek.error().to_invoke_error();
          }
          if (!peek.value().has_value() || peek.value().value().size() != 1) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        auto capped =
            ctx.handler().retrocausal_queue_consume(target, /*peek=*/false);
        if (!capped.has_value()) {
          return capped.error().to_invoke_error();
        }
        // Expect the handleable read-limit error, not an entry.
        if (capped.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (capped.value().error().code() !=
            SyscallErrorCode::
                kSyscallErrorCodeRetrocausalQueueReadLimitReached) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto make_tx = [&](uint32_t method) {
    return Transaction{.invocation =
                           Invocation{.invoked_address = contract_addr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  };

  auto insert_tx = make_tx(10);
  auto consume_tx = make_tx(11);
  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;

  // Proposal: run and commit both txs, then capture the would-be post-block
  // DbHash. discard_block closes the block (finalizing the trie root), captures
  // the hash, and abandons it, so validation replays against the same pre-block
  // state.
  {
    auto insert_handler = make_new_system_call_handler(insert_tx, *context);
    ASSERT_TRUE(insert_handler.root_invoke().is_root_success());
    insert_proposal_data = insert_handler.get_proposal_data();
    ASSERT_EQ(insert_handler.try_commit_transaction(),
              ExecResult::ExecResultOK);

    auto consume_handler = make_new_system_call_handler(consume_tx, *context);
    ASSERT_TRUE(consume_handler.root_invoke().is_root_success());
    consume_proposal_data = consume_handler.get_proposal_data();
    // The cap-crossing consume logs a nondeterministic bit but records no
    // observation: the sequence stops at the cap while the bit count is one
    // greater.
    ASSERT_EQ(consume_proposal_data.observed_queue_entry_sequence.size(),
              kMaxConcurrentRcqReadsPerTx);
    ASSERT_EQ(consume_proposal_data.nondeterministic_bit_count,
              kMaxConcurrentRcqReadsPerTx + 1);
    ASSERT_EQ(consume_handler.try_commit_transaction(),
              ExecResult::ExecResultOK);
  }

  auto proposal_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: replay both txs from the recorded proposal data, commit, and
  // capture the post-block DbHash.
  {
    auto insert_handler =
        make_new_system_call_handler_validation(insert_tx, *context,
                                                insert_proposal_data);
    ASSERT_TRUE(insert_handler.root_invoke().is_root_success());
    ASSERT_EQ(insert_handler.try_commit_transaction(),
              ExecResult::ExecResultOK);

    auto consume_handler =
        make_new_system_call_handler_validation(consume_tx, *context,
                                                consume_proposal_data);
    ASSERT_TRUE(consume_handler.root_invoke().is_root_success());
    ASSERT_EQ(consume_handler.try_commit_transaction(),
              ExecResult::ExecResultOK);
  }

  auto validation_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  EXPECT_TRUE(proposal_hash == validation_hash)
      << "proposal and validation must commit identical RCQ deltas after a "
         "contract handles the RCQ observation read-limit error (#3043)";
}

// #3611: The per-tx observation cap bounds only concurrent observations. A
// self-inserted entry records no observation, so consuming it must succeed and
// return its byte even once the cap is exceeded. Pre-fix, load_next_queue_entry
// converted the read into kSyscallErrorCodeRetrocausalQueueReadLimitReached
// after get_next_item had already advanced the materialized queue item, so the
// contract got an error while the consume silently committed -- the byte was
// dropped. This test crosses the cap, consumes a self entry, and asserts the
// consume returns the byte and (from a later tx) actually committed.
TEST_F(SyscallRcqPeekTest, SelfConsumeSucceedsAtObservationCap)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, int128_t{kHogDivisibility} *
                                               int128_t{kTotalHogSupply});

  NativeContractBuilder builder;
  // method 12: insert one entry into each of kMaxConcurrentRcqReadsPerTx + 1
  // distinct filler queues. Committing this tx first makes them current-block
  // concurrent entries the attack tx below can observe to fill the cap.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      12, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx + 1; ++i) {
          auto payload = static_cast<uint8_t>(i);
          auto res = ctx.handler().retrocausal_queue_insert(
              make_obs_cap_key(i), std::as_bytes(std::span(&payload, 1)));
          if (!res.has_value()) {
            return res.error().to_invoke_error();
          }
          if (!res.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      });
  // method 13: peek the fillers to fill the observation sequence to the cap and
  // then cross it (setting the sticky cap flag), insert a self entry, then
  // destructively consume that self entry. The self consume records no
  // observation, so it must succeed and return the byte despite the cap.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      13, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto self_key =
            NativeContractTestFramework::make_storage_key("self_rcq");
        for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx + 1; ++i) {
          auto peek =
              ctx.handler().retrocausal_queue_consume(make_obs_cap_key(i),
                                                      /*peek=*/true);
          if (!peek.has_value()) {
            return peek.error().to_invoke_error();
          }
          if (i < kMaxConcurrentRcqReadsPerTx) {
            if (!peek.value().has_value() || peek.value().value().size() != 1) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
          }
          // The cap-crossing peek returns the read-limit error; the sticky flag
          // is now set for the rest of the tx.
          else if (peek.value().has_value() ||
                   peek.value().error().code() !=
                       SyscallErrorCode::
                           kSyscallErrorCodeRetrocausalQueueReadLimitReached) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        uint8_t secret = 0xA5;
        auto insert = ctx.handler().retrocausal_queue_insert(
            self_key, std::as_bytes(std::span(&secret, 1)));
        if (!insert.has_value()) {
          return insert.error().to_invoke_error();
        }
        if (!insert.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto consume =
            ctx.handler().retrocausal_queue_consume(self_key, /*peek=*/false);
        if (!consume.has_value()) {
          return consume.error().to_invoke_error();
        }
        // The self entry is returned, not dropped behind a read-limit error.
        if (!consume.value().has_value() ||
            consume.value().value().size() != 1) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  // method 14: a fresh tx confirms the self entry was actually consumed (the
  // successful consume committed), so the queue is now empty.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      14, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto self_key =
            NativeContractTestFramework::make_storage_key("self_rcq");
        auto peek =
            ctx.handler().retrocausal_queue_consume(self_key, /*peek=*/true);
        if (!peek.has_value()) {
          return peek.error().to_invoke_error();
        }
        if (peek.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (peek.value().error().code() !=
                SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry &&
            peek.value().error().code() !=
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto run = [&](uint32_t method) {
    auto tx = Transaction{.invocation =
                              Invocation{.invoked_address = contract_addr,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()};
    auto handler = make_new_system_call_handler(tx, *context);
    return handler.root_invoke().is_root_success() &&
           handler.try_commit_transaction() == ExecResult::ExecResultOK;
  };

  ASSERT_TRUE(run(12));
  ASSERT_TRUE(run(13));
  EXPECT_TRUE(run(14))
      << "the self entry should have been consumed and committed by the "
         "at-cap consume that returned it";
}

// #3611: The same holds for a past-block consume through the materialized-item
// path (the issue names both self and past-block entries). A past-block read
// records no observation, so consuming it must succeed and return its bytes
// even once the cap is exceeded. A queue that carries a committed past-block
// entry and is self-inserted into by the same tx is a materialized
// RetrocausalQueueItem; pre-fix the capped consume advanced its past-block
// cursor in place and returned a read-limit error, committing a consume of the
// past-block entry the contract never saw.
TEST_F(SyscallRcqPeekTest, PastBlockConsumeSucceedsAtObservationCap)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, int128_t{kHogDivisibility} *
                                               int128_t{kTotalHogSupply});

  NativeContractBuilder builder;
  // method 20: insert a two-byte past-block entry into the target queue.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      20, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto target =
            NativeContractTestFramework::make_storage_key("pastblk_q");
        std::array<uint8_t, 2> past = {0xE0, 0xE1};
        auto res = ctx.handler().retrocausal_queue_insert(target,
                                                          std::as_bytes(
                                                              std::span(past)));
        if (!res.has_value()) {
          return res.error().to_invoke_error();
        }
        if (!res.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  // method 21: insert one entry into each of kMaxConcurrentRcqReadsPerTx + 1
  // filler queues (current-block concurrent entries for the attack tx).
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      21, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx + 1; ++i) {
          auto payload = static_cast<uint8_t>(i);
          auto res = ctx.handler().retrocausal_queue_insert(
              make_obs_cap_key(i), std::as_bytes(std::span(&payload, 1)));
          if (!res.has_value()) {
            return res.error().to_invoke_error();
          }
          if (!res.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        return {};
      });
  // method 22: cross the cap, self-insert a one-byte entry into the target
  // (materializing it), then destructively consume it. The past-block entry is
  // next in consume order; the consume must succeed and return that two-byte
  // entry despite the cap.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      22, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto target =
            NativeContractTestFramework::make_storage_key("pastblk_q");
        for (uint32_t i = 0; i < kMaxConcurrentRcqReadsPerTx + 1; ++i) {
          auto peek =
              ctx.handler().retrocausal_queue_consume(make_obs_cap_key(i),
                                                      /*peek=*/true);
          if (!peek.has_value()) {
            return peek.error().to_invoke_error();
          }
          if (i < kMaxConcurrentRcqReadsPerTx) {
            if (!peek.value().has_value() || peek.value().value().size() != 1) {
              return InvokeError{InvokeErrorCode::Deterministic};
            }
          }
          else if (peek.value().has_value() ||
                   peek.value().error().code() !=
                       SyscallErrorCode::
                           kSyscallErrorCodeRetrocausalQueueReadLimitReached) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        uint8_t self_byte = 0xA5;
        auto insert = ctx.handler().retrocausal_queue_insert(
            target, std::as_bytes(std::span(&self_byte, 1)));
        if (!insert.has_value()) {
          return insert.error().to_invoke_error();
        }
        if (!insert.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto consume =
            ctx.handler().retrocausal_queue_consume(target, /*peek=*/false);
        if (!consume.has_value()) {
          return consume.error().to_invoke_error();
        }
        // The two-byte past-block entry is returned, not dropped.
        if (!consume.value().has_value() ||
            consume.value().value().size() != 2) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  // method 23: a fresh tx consumes the target again. The past-block entry was
  // legitimately consumed above, so the only entry left is the one-byte self
  // entry -- its length confirms the past-block consume committed and the self
  // insert persisted.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      23, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto target =
            NativeContractTestFramework::make_storage_key("pastblk_q");
        auto consume =
            ctx.handler().retrocausal_queue_consume(target, /*peek=*/false);
        if (!consume.has_value()) {
          return consume.error().to_invoke_error();
        }
        if (!consume.value().has_value() ||
            consume.value().value().size() != 1) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto run = [&](uint32_t method) {
    auto tx = Transaction{.invocation =
                              Invocation{.invoked_address = contract_addr,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()};
    auto handler = make_new_system_call_handler(tx, *context);
    return handler.root_invoke().is_root_success() &&
           handler.try_commit_transaction() == ExecResult::ExecResultOK;
  };

  ASSERT_TRUE(run(20));
  // Close the block so the target entry becomes a past-block entry.
  advance_block();
  ASSERT_TRUE(run(21));
  ASSERT_TRUE(run(22));
  // Close the block again so the remaining entries are all past-block: the
  // verifying consume is then deterministic and independent of concurrent
  // observation.
  advance_block();
  EXPECT_TRUE(run(23))
      << "the past-block entry should have been consumed and committed by the "
         "at-cap consume that returned it, leaving only the self entry";
}

// #3611: Suppressing the consume advance applies only at the cap. A below-cap
// destructive consume through the materialized-item path must still advance and
// commit normally -- this guards against the fix over-rejecting or failing to
// consume when the cap has not been reached.
TEST_F(SyscallRcqPeekTest, MaterializedSelfConsumeCommitsWhenBelowCap)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  NativeContractBuilder builder;
  // method 30: insert a self entry into the queue (materializing it), then
  // destructively consume it -- the entry must be returned.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      30, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto key = NativeContractTestFramework::make_storage_key("belowcap_q");
        uint8_t payload = 0xC3;
        auto insert = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&payload, 1)));
        if (!insert.has_value()) {
          return insert.error().to_invoke_error();
        }
        if (!insert.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto consume =
            ctx.handler().retrocausal_queue_consume(key, /*peek=*/false);
        if (!consume.has_value()) {
          return consume.error().to_invoke_error();
        }
        if (!consume.value().has_value() ||
            consume.value().value().size() != 1) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  // method 31: a fresh tx confirms the entry was actually consumed (the
  // successful consume committed), so the queue is now empty.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      31, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto key = NativeContractTestFramework::make_storage_key("belowcap_q");
        auto peek = ctx.handler().retrocausal_queue_consume(key, /*peek=*/true);
        if (!peek.has_value()) {
          return peek.error().to_invoke_error();
        }
        if (peek.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (peek.value().error().code() !=
                SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry &&
            peek.value().error().code() !=
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder.build());
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto run = [&](uint32_t method) {
    auto tx = Transaction{.invocation =
                              Invocation{.invoked_address = contract_addr,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()};
    auto handler = make_new_system_call_handler(tx, *context);
    return handler.root_invoke().is_root_success() &&
           handler.try_commit_transaction() == ExecResult::ExecResultOK;
  };

  ASSERT_TRUE(run(30));
  EXPECT_TRUE(run(31))
      << "a below-cap consume must actually commit the consume";
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
