#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

class RCQDeleteTest : public ::testing::TestWithParam<TestDB>,
                      public NativeContractTestFramework {
protected:
  RCQDeleteTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

  std::vector<uint8_t> last_consumed_values_;

  inline static std::atomic<bool> wait_flag_1_ = false;
  inline static std::atomic<bool> wait_flag_2_ = false;

  constexpr static auto kStorageKey = make_storage_key("rcq");
  constexpr static ContractAddress kContractAddress =
      make_contract_address(0); // Default address

  static AddressAndKey get_queue_addrkey()
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() = kStorageKey;
    return AddressAndKey{.address = kContractAddress, .key = key};
  }

  static std::unique_ptr<RuntimeContext> make_context()
  {
    NativeContractBuilder builder;
    builder.add<SerializationFormat::kTxs>(
        0,
        [](SystemCallHandler &handler, uint32_t value) -> InvokeStatus<void> {
          // insert an entry to the queue
          auto [res, _] = syscall_RetrocausalQueueInsert_inner(
              handler, kStorageKey, std::as_bytes(std::span(&value, 1)));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "failed to insert element to RCQ";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          return {};
        });

    builder.add<SerializationFormat::kTxs>(
        1,
        [](SystemCallHandler &handler, uint32_t value) -> InvokeStatus<void> {
          // consume an entry, and check that against the supplied input
          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;

          auto [res, _] = syscall_RetrocausalQueueConsume_inner(
              handler, kStorageKey, false,
              std::as_writable_bytes(std::span(&out, 1)), written_len);
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "failed to consume element to RCQ";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          if (written_len != 4 || out != value) {
            ADD_FAILURE() << "Got bad result from RCQ: expected " << value
                          << " but got " << out;
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          return {};
        });

    builder.add<SerializationFormat::kTxs>(
        2, [](SystemCallHandler &handler) -> InvokeStatus<void> {
          // consume an entry, expect to get nothing
          uint32_t out = 0;
          RuntimeMemoryLength written_len = 0;

          auto [res, _] = syscall_RetrocausalQueueConsume_inner(
              handler, kStorageKey, false,
              std::as_writable_bytes(std::span(&out, 1)), written_len);
          if (!res.has_value() ||
              ((res.value() !=
                SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) &&
               (res.value() !=
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist))) {
            ADD_FAILURE() << "Got unexpected item from rcq";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          if (written_len != 0) {
            ADD_FAILURE() << "Got bad result from RCQ";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          return {};
        });

    builder.add<SerializationFormat::kTxs>(
        3,
        [](SystemCallHandler &handler,
           Bytestring bytestring) -> InvokeStatus<void> {
          // Write a bytestring
          auto [res, _] = syscall_BytestringSet_inner(
              handler, kStorageKey, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!res.has_value() ||
              res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to set bytestring";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          return {};
        });

    // Test for NoDoubleConsumeDeletedEntries:
    // Consume -> Delete -> Consume

    builder.add<SerializationFormat::kTxs>(
        10,
        [](SystemCallHandler &handler, uint32_t consume_a,
           uint32_t consume_b) -> InvokeStatus<void> {
          // consume -> delete -> consume

          auto [r1, _g1] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          auto [r3, _g3] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume";
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          return {};
        });

    // Test for NoDeleteUnobservedEntries
    builder.add<SerializationFormat::kTxs>(
        11,
        [](SystemCallHandler &handler,
           uint32_t consume_b) -> InvokeStatus<void> {
          // consume (nothing observed) -> delete -> consume

          auto [r1, _g1] =
              handler.invoke(kContractAddress, 2,
                             std::numeric_limits<GasAmount>::max(), {},
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume(expect none)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          wait_flag_1_ = true;
          wait_flag_1_.notify_all();

          wait_flag_2_.wait(false);

          auto [r3, _g3] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // Test for DeleteSelfInsertedEntry
    builder.add<SerializationFormat::kTxs>(
        12,
        [](SystemCallHandler &handler, uint32_t insert_a,
           uint32_t consume_b) -> InvokeStatus<void> {
          // insert() -> delete -> consume

          auto [r1, _g1] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke insert";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r3, _g3] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // Test for DeleteAndRecreateRemovesPastBlocks
    builder.add<SerializationFormat::kTxs>(
        13,
        [](SystemCallHandler &handler,
           uint32_t insert_1) -> InvokeStatus<void> {
          // delete -> insert()
          auto [r1, _g1] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_1),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke insert";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // ConsumeOneOfManyDeleteConsumeRest:
    // consume(a) -> delete -> consume(b) -> consume(c)
    builder.add<SerializationFormat::kTxs>(
        14,
        [](SystemCallHandler &handler, uint32_t consume_a, uint32_t consume_b,
           uint32_t consume_c) -> InvokeStatus<void> {
          auto [r1, _g1] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume(a)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r3, _g3] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume(b)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r4, _g4] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_c),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume(c)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // InsertSelfConsumeSelfDeleteInsertConsume:
    // insert(a) -> consume(a) -> delete -> insert(b) -> consume(b)
    builder.add<SerializationFormat::kTxs>(
        15,
        [](SystemCallHandler &handler, uint32_t insert_a,
           uint32_t insert_b) -> InvokeStatus<void> {
          // insert(a)
          auto [r1, _g1] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert(a)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume(a)
          auto [r2, _g2] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume(a)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // delete
          auto [r3, _g3] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // insert(b)
          auto [r4, _g4] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert(b)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume(b)
          auto [r5, _g5] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r5.has_value() ||
              r5.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume(b)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // DeleteConsumeEmpty: delete -> consume(expect nothing)
    builder.add<SerializationFormat::kTxs>(
        17, [](SystemCallHandler &handler) -> InvokeStatus<void> {
          auto [r1, _g1] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] =
              handler.invoke(kContractAddress, 2,
                             std::numeric_limits<GasAmount>::max(), {},
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume_empty";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // ConsumeEmptyDeleteConsumeEmpty:
    // consume(nothing) -> delete -> consume(nothing)
    builder.add<SerializationFormat::kTxs>(
        18, [](SystemCallHandler &handler) -> InvokeStatus<void> {
          auto [r1, _g1] =
              handler.invoke(kContractAddress, 2,
                             std::numeric_limits<GasAmount>::max(), {},
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume_empty (first)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r3, _g3] =
              handler.invoke(kContractAddress, 2,
                             std::numeric_limits<GasAmount>::max(), {},
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to invoke consume_empty (second)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // MultipleDeleteRecreate:
    // consume(a) -> delete -> consume(b) -> delete -> consume(c)
    builder.add<SerializationFormat::kTxs>(
        19,
        [](SystemCallHandler &handler, uint32_t consume_a, uint32_t consume_b,
           uint32_t consume_c) -> InvokeStatus<void> {
          auto [r1, _g1] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume(a)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue (first)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r3, _g3] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume(b)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r4, _g4] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue (second)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          auto [r5, _g5] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_c),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r5.has_value() ||
              r5.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume(c)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // DeleteInsertDeleteInsertConsume:
    // delete -> insert(a) -> delete -> insert(b) -> consume(concurrent)
    builder.add<SerializationFormat::kTxs>(
        20,
        [](SystemCallHandler &handler, uint32_t insert_a, uint32_t insert_b,
           uint32_t consume_c) -> InvokeStatus<void> {
          // first delete
          auto [r1, _g1] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue (first)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // insert(a)
          auto [r2, _g2] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_a),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert(a)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // second delete
          auto [r3, _g3] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue (second)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // insert(b)
          auto [r4, _g4] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert(b)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume(b) -- self entry consumed before concurrent
          auto [r5, _g5] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_b),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r5.has_value() ||
              r5.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume self(b)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume(c) -- concurrent entry
          auto [r6, _g6] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_c),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r6.has_value() ||
              r6.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume(c)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // PeekDeleteConsume:
    // peek(expected) -> delete -> consume(expected)
    builder.add<SerializationFormat::kTxs>(
        21,
        [](SystemCallHandler &handler,
           uint32_t expected) -> InvokeStatus<void> {
          // peek
          uint32_t peeked = 0;
          RuntimeMemoryLength peek_len = 0;
          auto [rp, _gp] = syscall_RetrocausalQueueConsume_inner(
              handler, kStorageKey, true,
              std::as_writable_bytes(std::span(&peeked, 1)), peek_len);
          if (!rp.has_value() ||
              rp.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to peek";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }
          if (peek_len != 4 || peeked != expected) {
            ADD_FAILURE() << "Peek: expected " << expected << " but got "
                          << peeked;
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          // delete
          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume (should get same entry since peek doesn't advance)
          auto [r3, _g3] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(expected),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume after peek+delete";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // ConsumePastBlockDeleteConsumeConcurrent:
    // consume(past_entry) -> delete -> insert(new) -> consume(self) ->
    // consume(concurrent)
    builder.add<SerializationFormat::kTxs>(
        22,
        [](SystemCallHandler &handler, uint32_t past_value,
           uint32_t insert_value,
           uint32_t concurrent_value) -> InvokeStatus<void> {
          // consume past block entry
          auto [r1, _g1] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(past_value),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume past block entry";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // delete
          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // insert new entry
          auto [r3, _g3] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_value),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume self entry (self entries consumed before concurrent)
          auto [r4, _g4] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_value),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume self entry";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // consume concurrent entry
          auto [r5, _g5] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(concurrent_value),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r5.has_value() ||
              r5.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume concurrent entry";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    // ChangeTypeAndRecreateFromEmpty:
    // consume(retrocausal) -> delete -> create bytestring -> delete ->
    // create nnint -> delete -> insert(item)
    builder.add<SerializationFormat::kTxs>(
        23,
        [](SystemCallHandler &handler, uint32_t consume_value,
           uint32_t insert_value) -> InvokeStatus<void> {
          // consume retrocausal item
          auto [r1, _g1] =
              handler.invoke(kContractAddress, 1,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(consume_value),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r1.has_value() ||
              r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to consume retrocausal item";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // delete
          auto [r2, _g2] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete queue (first)";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // create bytestring
          Bytestring bytestring =
              NativeContractTestFramework::make_bytestring("test");
          auto [r3, _g3] = syscall_BytestringSet_inner(
              handler, kStorageKey, std::as_bytes(std::span(bytestring)),
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r3.has_value() ||
              r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to create bytestring";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // delete
          auto [r4, _g4] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r4.has_value() ||
              r4.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete bytestring";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // create nnint
          int128_t set_value = 42;
          auto [r5, _g5] = syscall_NonnegativeIntSetAdd_inner(
              handler, kStorageKey, &set_value, 0,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r5.has_value() ||
              r5.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to create nnint";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // delete
          auto [r6, _g6] = syscall_DeleteObject_inner(
              handler, kStorageKey,
              std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
          if (!r6.has_value() ||
              r6.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to delete nnint";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          // insert item
          auto [r7, _g7] =
              handler.invoke(kContractAddress, 0,
                             std::numeric_limits<GasAmount>::max(),
                             x23::serialize(insert_value),
                             std::to_underlying(InvokeFlag::kInvokeFlagNone));
          if (!r7.has_value() ||
              r7.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "Failed to insert item";
            return InvokeError{InvokeErrorCode::kInvokeNone};
          }

          return {};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, builder.build());
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  void SetUp() override
  {
    // mark the contract as existing and deployed within the database
    auto contract_0 = make_native_contract(0);
    write_contract(contract_0);
    write_deployment(kContractAddress, contract_0);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
    context_ = make_context();

    wait_flag_1_ = false;
    wait_flag_2_ = false;
  }

  template<x23::is_xdr_type... Args>
  void invoke_method(Runtime::MethodID method, Args const &...args)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = kContractAddress,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = x23::serialize(args...)},
                }},
        *context_);

    auto r = handler.root_invoke();
    ASSERT_TRUE(r.is_root_success());
    auto res = handler.try_commit_transaction();
    ASSERT_EQ(res, ExecResult::ExecResultOK);
  }

  template<x23::is_xdr_type... Args>
  void invoke_method_fail_commit(Runtime::MethodID method, Args const &...args)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = kContractAddress,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = x23::serialize(args...)},
                }},
        *context_);

    auto r = handler.root_invoke();
    ASSERT_TRUE(r.is_root_success());
    auto res = handler.try_commit_transaction();
    ASSERT_NE(res, ExecResult::ExecResultOK);
  }


  void insert(uint32_t value) { invoke_method(0, value); }

  void set_bytestring(Bytestring const &bytestring)
  {
    invoke_method(3, bytestring);
  }

  std::unique_ptr<RuntimeContext> context_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_P(RCQDeleteTest, NoDoubleConsumeDeletedEntries)
{
  insert(42);
  insert(99);
  invoke_method(10, uint32_t{42}, uint32_t{99});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
  // TODO(geoff): assertion on qe.max_live_block_num
}

TEST_P(RCQDeleteTest, NoDeleteUnobservedEntries)
{
  std::thread t([&] { invoke_method(11, uint32_t{42}); });

  wait_flag_1_.wait(false);

  insert(42);

  wait_flag_2_ = true;
  wait_flag_2_.notify_all();
  t.join();

  advance_block();
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

TEST_P(RCQDeleteTest, DeleteSelfInsertedEntry)
{
  insert(42);

  invoke_method(12, uint32_t{99}, uint32_t{42});

  advance_block();
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

TEST_P(RCQDeleteTest, DeleteAndRecreateRemovesPastBlocks)
{
  insert(42);
  insert(99);

  advance_block();

  invoke_method(13, uint32_t{10});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);
  ASSERT_EQ(qe.max_live_block_num,
            2); // TODO(geoff): is this assertion correct?   This matches in
                // hogdb and mockdb, so I guess so?
}

// Consume 1 of 3 concurrent entries, delete, consume remaining 2.
// Exposes the bug where current_block_consumed lost entries across delete.
TEST_P(RCQDeleteTest, ConsumeOneOfManyDeleteConsumeRest)
{
  insert(42);
  insert(99);
  insert(77);
  invoke_method(14, uint32_t{42}, uint32_t{99}, uint32_t{77});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Consume entry from past block, delete queue, insert new entry,
// consume concurrent entry.
TEST_P(RCQDeleteTest, ConsumePastBlockDeleteConsumeConcurrent)
{
  // Block 1: insert entries that become past block entries
  insert(42);

  advance_block();

  // Block 2: concurrent insert + our method
  insert(99);
  invoke_method(22, uint32_t{42}, uint32_t{10}, uint32_t{99});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  // Past block entry (42) consumed via make_fully_consumed_item,
  // self insert (10) consumed, concurrent (99) consumed.
  ASSERT_EQ(qe.live_entries, 0);
}

// Insert and consume a self entry, delete, insert and consume another self.
// Verifies self entries before delete are discarded correctly.
TEST_P(RCQDeleteTest, InsertSelfConsumeSelfDeleteInsertConsume)
{
  invoke_method(15, uint32_t{42}, uint32_t{99});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Insert a self entry, delete, then consume a concurrent entry.
// Verifies self insert is gone, concurrent is consumed.
TEST_P(RCQDeleteTest, InsertSelfDeleteConsumeConcurrent)
{
  insert(42);
  invoke_method(12, uint32_t{99}, uint32_t{42});

  advance_block();
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Delete queue, then try to consume. Should get no entry.
// Insert is in previous block so no concurrent entries are available.
TEST_P(RCQDeleteTest, DeleteConsumeEmpty)
{
  insert(42);

  advance_block();

  invoke_method(17);

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Consume (nothing available) -> delete -> consume (nothing available).
// Both should return no entry. retrocausally_consumed_count_ should remain 0.
TEST_P(RCQDeleteTest, ConsumeEmptyDeleteConsumeEmpty)
{
  invoke_method(18);

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Multiple delete-recreate cycles: consume(a) -> delete -> consume(b) -> delete
// -> consume(c). Verifies that preserved entries accumulate correctly across
// multiple cycles.
TEST_P(RCQDeleteTest, MultipleDeleteRecreate)
{
  insert(42);
  insert(99);
  insert(77);
  invoke_method(19, uint32_t{42}, uint32_t{99}, uint32_t{77});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Two delete-recreate cycles without consumes in between,
// then consume a concurrent entry at the end.
TEST_P(RCQDeleteTest, DeleteInsertDeleteInsertConsume)
{
  insert(42);
  invoke_method(20, uint32_t{10}, uint32_t{20}, uint32_t{42});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  // Self entry 10 destroyed by second delete, self entry 20 consumed,
  // concurrent 42 consumed.
  ASSERT_EQ(qe.live_entries, 0);
}

// Peek at concurrent entry, delete, consume. Since peek doesn't advance
// the cursor, retrocausally_consumed_count_ stays 0. After delete,
// consume should return the same entry (index 0).
TEST_P(RCQDeleteTest, PeekDeleteConsume)
{
  insert(42);
  invoke_method(21, uint32_t{42});

  advance_block();

  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Start with an empty slot.  Consumes a retrocausal item, then deletes,
// then creates a bytestring, then deletes, then creates an nnint,
// then deletes, then inserts an item.  Verify that at the next block there's
// only the one item inserted at the end
TEST_P(RCQDeleteTest, ChangeTypeAndRecreateFromEmpty)
{
  // Insert an item in a previous block so it becomes retrocausal
  insert(42);

  // In this block: consume retrocausal, delete, create bytestring, delete,
  // create nnint, delete, insert new item
  invoke_method(23, uint32_t{42}, uint32_t{99});

  advance_block();

  // Verify only the inserted item remains
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);

  // Verify that the one item left is the one inserted at the end
  invoke_method(1, uint32_t{99});
}

// Start with a nonempty slot.  Consumes an item, then deletes,
// then creates a bytestring, then deletes, then creates an nnint,
// then deletes, then inserts an item.  Verify that at the next block there's
// only the one item inserted at the end
TEST_P(RCQDeleteTest, ChangeTypeAndRecreateFromNonEmpty)
{
  // Insert an item in a previous block
  insert(42);

  advance_block();

  // In this block: consume retrocausal, delete, create bytestring, delete,
  // create nnint, delete, insert new item
  invoke_method(23, uint32_t{42}, uint32_t{99});

  advance_block();

  // Verify only the inserted item remains
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);

  // Verify that the one item left is the one inserted at the end
  invoke_method(1, uint32_t{99});
}

// Test that we only output the consume if entries are actually consumed.
// Starts from empty, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromEmpty)
{
  // Start from empty, delete (no-op), then insert a new item
  invoke_method(13, uint32_t{42});

  // Then retrocausally consume the item
  invoke_method(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Test that we only output the consume if entries are actually consumed.
// Starts from empty, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromBytestring)
{
  set_bytestring(make_bytestring("test"));
  advance_block();
  // delete, then insert a new item
  invoke_method(13, uint32_t{42});

  // Then retrocausally consume the item
  invoke_method(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}

// Test that we only output the consume if entries are actually consumed.
// Starts from empty, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromEmptyQueue)
{
  insert(42);
  invoke_method(1, uint32_t{42});

  advance_block();
  // delete, then insert a new item
  invoke_method(13, uint32_t{42});

  // Then retrocausally consume the item
  invoke_method(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 0);
}
// Test that we only output the consume if entries are actually consumed.
// Starts from nonempty queue, then deletes the item, then inserts a new item.
// Then, another transaction should successfully be able to (retrocausally)
// consume that item.
TEST_P(RCQDeleteTest, ConsumeOnlyWhenNecessaryStartFromNonEmptyQueue)
{
  insert(42);

  advance_block();
  // delete, then insert a new item
  invoke_method(13, uint32_t{99});

  // Consuming the preexisting item should fail
  invoke_method_fail_commit(1, uint32_t{42});

  advance_block();

  // Verify the queue is now empty (item was consumed)
  auto queue = lookup_storage_item_from_db(get_queue_addrkey());
  ASSERT_TRUE(queue.has_value());
  ASSERT_TRUE(queue->type() == StorageItemType::kItemTypeRetrocausalQueue);
  auto const &qe = queue->metadata();
  ASSERT_EQ(qe.live_entries, 1);
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQDeleteTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
