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
            ADD_FAILURE() << "Got bad result from RCQ";
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

  void insert(uint32_t value) { invoke_method(0, value); }

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
            1); // TODO(geoff): is this assertion correct?
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, RCQDeleteTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
