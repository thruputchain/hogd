#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// Tests for nnint get syscall with force_load_snapshot=false.
// This tests edge cases where we don't force loading from the snapshot,
// focusing on the behavior when keys don't exist, are deleted, have different
// types, or have been written to in the current transaction.

class NNIntGetTest : public ::testing::TestWithParam<TestDB>,
                     public NativeContractTestFramework {
public:
  NNIntGetTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  static InvokeStatus<void>
  check_nnint_get_result(std::optional<int128_t> res_base, int128_t res_add,
                         std::optional<int128_t> expected_base,
                         int128_t expected_add)
  {
    if (res_base != expected_base) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (res_add != expected_add) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  }
};

// Test: Get on a key that doesn't exist (UnmodifiedItem, not loaded).
// With force_load_snapshot=false, should return (nullopt, 0) without
// querying the database.
TEST_P(NNIntGetTest, GetOnNonExistentKey)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("nonexistent");
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt
                                      ? std::optional<int128_t>(output_set)
                                      : std::nullopt,
                                  output_add, std::nullopt, 0);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that was deleted (DeletedItem).
// With force_load_snapshot=false, should return (nullopt, 0).
TEST_P(NNIntGetTest, GetOnDeletedKey)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("deleted_key");
    // First, set a value
    int128_t set_value = 100;
    auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 50, std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Then delete it
    auto r2 = ctx.syscall<SyscallNo::kDeleteObject>(
        key, std::to_underlying(StorageDeltaFlags::None));
    if (!r2.has_value() || *r2 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get with force_load_snapshot=false should return (nullopt, 0)
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt
                                      ? std::optional<int128_t>(output_set)
                                      : std::nullopt,
                                  output_add, std::nullopt, 0);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that has an NNInt write with base_value.
// Should return (base_value, add_value).
TEST_P(NNIntGetTest, GetOnNNIntWithBaseValue)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("nnint_with_base");
    int128_t set_value = 200;
    auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 50, std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get with force_load_snapshot=false should return (200, 50)
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt
                                      ? std::optional<int128_t>(output_set)
                                      : std::nullopt,
                                  output_add, 200, 50);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that has an NNInt write without base_value (just add).
// Should return (nullopt, add_value).
TEST_P(NNIntGetTest, GetOnNNIntWithoutBaseValue)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("nnint_add_only");
    // Set with nullopt base, just add value
    auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, static_cast<int128_t const *>(nullptr), 75,
        std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get with force_load_snapshot=false should return (nullopt, 75)
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt
                                      ? std::optional<int128_t>(output_set)
                                      : std::nullopt,
                                  output_add, std::nullopt, 75);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that has a bytestring write (different type).
// With force_load_snapshot=false, should return (nullopt, 0).
TEST_P(NNIntGetTest, GetOnBytestringKey)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("bytestring_key");
    // First, set a bytestring value
    Bytestring bytestring_data = make_bytestring("test_data");
    auto r = ctx.syscall<SyscallNo::kBytestringSet>(
        key, std::as_bytes(std::span(bytestring_data)),
        std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get with force_load_snapshot=false should return (nullopt, 0)
    // because the type doesn't match
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt
                                      ? std::optional<int128_t>(output_set)
                                      : std::nullopt,
                                  output_add, std::nullopt, 0);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Get on a key that exists in snapshot but hasn't been accessed yet.
// With force_load_snapshot=false, should return (nullopt, 0) without
// loading from snapshot.
TEST_P(NNIntGetTest, GetOnUnmodifiedKeyInSnapshot)
{
  auto contract_addr = make_contract_address(0);
  auto key = make_contract_data_addrkey(contract_addr, "snapshot_key");

  // First, write an NNInt to the database in a previous block
  write_item(key,
             std::make_shared<StorageItemWrapper>(300,
                                                  get_default_cost_model()));
  advance_block();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("snapshot_key");
    // Get with force_load_snapshot=false should return (nullopt, 0)
    // without loading from snapshot
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt
                                      ? std::optional<int128_t>(output_set)
                                      : std::nullopt,
                                  output_add, std::nullopt, 0);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Multiple operations - set with base, then add, then get.
// Verifies that the latest write is what's returned.
TEST_P(NNIntGetTest, MultipleOperationsSequence)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("multi_op_key");
    // First set with base value
    int128_t set_value = 100;
    auto r1 = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 10, std::to_underlying(StorageDeltaFlags::None));
    if (!r1.has_value() || *r1 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get should return (100, 10)
    int128_t output_set_1{};
    bool output_set_opt_1{};
    int128_t output_add_1{};
    auto res1 =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set_1,
                                                   output_set_opt_1,
                                                   output_add_1);
    if (!res1.has_value() || *res1 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (auto err =
            check_nnint_get_result(output_set_opt_1
                                       ? std::optional<int128_t>(output_set_1)
                                       : std::nullopt,
                                   output_add_1, 100, 10);
        !err) {
      return err;
    }

    // Then add without base (overwrites previous write)
    auto r2 = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, static_cast<int128_t const *>(nullptr), 25,
        std::to_underlying(StorageDeltaFlags::None));
    if (!r2.has_value() || *r2 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get should now return (nullopt, 25) - the latest write
    int128_t output_set_2{};
    bool output_set_opt_2{};
    int128_t output_add_2{};
    auto res2 =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, false, output_set_2,
                                                   output_set_opt_2,
                                                   output_add_2);
    if (!res2.has_value() || *res2 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return check_nnint_get_result(output_set_opt_2
                                      ? std::optional<int128_t>(output_set_2)
                                      : std::nullopt,
                                  output_add_2, std::nullopt, 25);
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: DeletedItem + force_load=true.
// Set a key, delete it, call kNonnegativeIntGet with force_load=true.
// Should return kSyscallErrorCodeStorageKeyNotExist.
TEST_P(NNIntGetTest, GetDeletedKeyWithForceLoad)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("force_load_deleted");
    // First, set a value
    int128_t set_value = 100;
    auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 50, std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Then delete it
    auto r2 = ctx.syscall<SyscallNo::kDeleteObject>(
        key, std::to_underlying(StorageDeltaFlags::None));
    if (!r2.has_value() || *r2 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get with force_load_snapshot=true on deleted item
    // should return kSyscallErrorCodeStorageKeyNotExist
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, true, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() ||
        *res != SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
      ADD_FAILURE() << "force_load on deleted item should return "
                       "kSyscallErrorCodeStorageKeyNotExist";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test: Non-NNInt type + force_load=true.
// Write a bytestring at a key, call kNonnegativeIntGet with force_load=true.
// Should return kSyscallErrorCodeInvalidSyscallInput.
TEST_P(NNIntGetTest, GetBytestringKeyWithForceLoad)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("force_load_bytestring");
    // First, set a bytestring value
    Bytestring bytestring_data = make_bytestring("test_data");
    auto r = ctx.syscall<SyscallNo::kBytestringSet>(
        key, std::as_bytes(std::span(bytestring_data)),
        std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // Get with force_load_snapshot=true on bytestring item
    // should return kSyscallErrorCodeInvalidSyscallInput
    int128_t output_set{};
    bool output_set_opt{};
    int128_t output_add{};
    auto res =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, true, output_set,
                                                   output_set_opt, output_add);
    if (!res.has_value() ||
        *res != SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
      ADD_FAILURE() << "force_load on bytestring item should return "
                       "kSyscallErrorCodeInvalidSyscallInput";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

INSTANTIATE_TEST_SUITE_P(TestDBs, NNIntGetTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
