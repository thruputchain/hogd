#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
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
          TestDB::kHogDB)
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

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
