#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class NNIntAddTest : public ::testing::TestWithParam<TestDB>,
                     public NativeContractTestFramework {
public:
  NNIntAddTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override {}

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

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_P(NNIntAddTest, ReadWriteAddValue)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("test");
    auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, static_cast<int128_t const *>(nullptr), 200,
        std::to_underlying(StorageDeltaFlags::None));
    if (!r.has_value() || *r != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

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
    if (auto err =
            check_nnint_get_result(output_set_opt_2
                                       ? std::optional<int128_t>(output_set_2)
                                       : std::nullopt,
                                   output_add_2, std::nullopt, 200);
        !err) {
      return err;
    }

    auto r3 = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, static_cast<int128_t const *>(nullptr), 300,
        std::to_underlying(StorageDeltaFlags::None));
    if (!r3.has_value() || *r3 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    // expected result: base 0, add 300
    int128_t output_set_4{};
    bool output_set_opt_4{};
    int128_t output_add_4{};
    auto res4 =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, true, output_set_4,
                                                   output_set_opt_4,
                                                   output_add_4);
    if (!res4.has_value() || *res4 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (auto err =
            check_nnint_get_result(output_set_opt_4
                                       ? std::optional<int128_t>(output_set_4)
                                       : std::nullopt,
                                   output_add_4, 0, 300);
        !err) {
      return err;
    }

    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
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
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 300);
}

// Test to assert consistency of behavior with regard to the implementation of
// add vs set_add.  Each one overwrites the other.
TEST_P(NNIntAddTest, ReadWriteAddOverwritesSetAdd)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("test");
    int128_t set_value = 200;
    auto res = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 300, std::to_underlying(StorageDeltaFlags::None));
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    auto res2 = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, static_cast<int128_t const *>(nullptr), 100,
        std::to_underlying(StorageDeltaFlags::None));
    if (!res2.has_value() || *res2 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }

    int128_t output_set_3{};
    bool output_set_opt_3{};
    int128_t output_add_3{};
    auto res3 =
        ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, true, output_set_3,
                                                   output_set_opt_3,
                                                   output_add_3);
    if (!res3.has_value() || *res3 != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (auto err =
            check_nnint_get_result(output_set_opt_3
                                       ? std::optional<int128_t>(output_set_3)
                                       : std::nullopt,
                                   output_add_3, 50, 100);
        !err) {
      return err;
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
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  write_item(make_contract_data_addrkey(contract_addr, "test"),
             std::make_shared<StorageItemWrapper>(50,
                                                  get_default_cost_model()));

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 150);
}

TEST_P(NNIntAddTest, SetAddWithAdd)
{
  // Create a native contract with two methods.  The first method calls set_add
  // on (0, 200). The second method calls add on (-100). Both methods should
  // assert that the syscalls succeed and then return success.

  // Then, create two transactions.  The first calls the first method,
  // and the second calls the second method.
  // Both transactions should succeed.

  // Then advance the block,
  // and check that the storage item is (100).

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, // First method: set_add on (0, 200)
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key = make_storage_key("test");
                  int128_t set_value = 0;
                  auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
                      key, &set_value, 200,
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!r.has_value() ||
                      *r != SyscallErrorCode::kSyscallErrorCodeNone) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
    fns.emplace(1, // Second method: add on (-100)
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key = make_storage_key("test");
                  auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
                      key, static_cast<int128_t const *>(nullptr), -100,
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!r.has_value() ||
                      *r != SyscallErrorCode::kSyscallErrorCodeNone) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);

  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // First transaction: call method 0 (set_add on (0, 200))
  auto handler1 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
              }},
      *context);

  auto res1 = handler1.root_invoke();
  ASSERT_TRUE(res1.is_root_success());
  auto commit_res1 = handler1.try_commit_transaction();
  ASSERT_EQ(commit_res1, ExecResult::ExecResultOK);

  // Second transaction: call method 1 (add on (-100))
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
              }},
      *context);

  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());
  auto commit_res2 = handler2.try_commit_transaction();
  // MockInMemory allows concurrent NNIntAdd(-100) alongside the set_add(0,200);
  // HogDB rejects it at the bounds pre-check (snapshot base=0, delta=-100 =>
  // sum < min(base,0)) and returns a conflict error.
  if (GetParam() == TestDB::kMockInMemory) {
    ASSERT_EQ(commit_res2, ExecResult::ExecResultOK);
  }
  else {
    ASSERT_EQ(commit_res2, ExecResult::ExecResultTxNonDeterministicError);
  }

  advance_block();

  // Check that the storage item reflects the committed state.
  // MockInMemory: both handlers committed => 0+200-100 = 100.
  // HogDB: only handler1 committed (handler2 conflicted) => 0+200 = 200.
  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  if (GetParam() == TestDB::kMockInMemory) {
    ASSERT_EQ(from_xdr(nnint_item), 100);
  }
  else {
    ASSERT_EQ(from_xdr(nnint_item), 200);
  }

  // Now assert that nnint add conflicts with set_add, if the set value differs
  // from the base value.
  auto handler3 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
              }},
      *context);

  auto res3 = handler3.root_invoke();
  ASSERT_TRUE(res3.is_root_success());
  auto commit_res3 = handler3.try_commit_transaction();
  ASSERT_EQ(commit_res3, ExecResult::ExecResultOK);

  // Fourth transaction: call method 1 (add on (-100))
  auto handler4 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier(),
              }},
      *context);

  auto res4 = handler4.root_invoke();
  ASSERT_TRUE(res4.is_root_success());
  auto commit_res4 = handler4.try_commit_transaction();
  ASSERT_EQ(commit_res4, ExecResult::ExecResultTxNonDeterministicError);

  // The getter surfaces the same diagnostic the FFI serializes out. handler4's
  // add conflicts: handler3 committed set=0 (locking base_=0 in HogDB), but
  // handler4's executor sees snapshot base=200, so the push is rejected with
  // kDbConflictValueMismatch.
  if (GetParam() == TestDB::kHogDB) {
    ASSERT_EQ(handler4.last_commit_conflict().level(),
              DbDiagnosticLevel::kDbDiagnosticAll);
    bool found_conflict = false;
    for (auto const &c : handler4.last_commit_conflict().conflicts()) {
      if (c.code == DbConflictCode::kDbConflictValueMismatch) {
        found_conflict = true;
      }
    }
    ASSERT_TRUE(found_conflict);
  }

  advance_block();

  // Check that the storage item is (200): handler3 committed set_add(0,200),
  // handler4 was rejected. Both backends agree on 200 here.
  read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "test"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 200);
}

// Regression test for #2155: negative set_value is intentionally allowed.
// The protocol invariant is value >= min(set_value, 0), not value >= 0.
TEST_P(NNIntAddTest, NegativeSetValueCommitsNegativeNNInt)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("neg_base");
    int128_t set_value = -1;
    auto res = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 0, std::to_underlying(StorageDeltaFlags::None));
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
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
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "neg_base"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), -1);
}

// Regression test for #2155: negative set_value with positive add_value.
// Final value is set_value + add_value.  The lock lower bound is
// min(set_value, 0), so the result is valid as long as it stays above that.
TEST_P(NNIntAddTest, NegativeSetValueWithPositiveAdd)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("neg_base_pos_add");
    int128_t set_value = -5;
    auto res = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, 10, std::to_underlying(StorageDeltaFlags::None));
    if (!res.has_value() || *res != SyscallErrorCode::kSyscallErrorCodeNone) {
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
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  auto read_res = lookup_storage_item_from_db(
      make_contract_data_addrkey(contract_addr, "neg_base_pos_add"));
  ASSERT_TRUE(read_res.has_value());
  ASSERT_EQ(read_res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
  auto nnint_item = read_res.value().nnint();
  ASSERT_EQ(from_xdr(nnint_item), 5);
}

// Regression test for #2155: negative set_value with negative add_value
// is rejected, since subtracting from an already-negative base would violate
// the lower bound.
TEST_P(NNIntAddTest, NegativeSetValueWithNegativeAddRejected)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto key = make_storage_key("neg_base_neg_add");
    int128_t set_value = -1;
    auto res = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
        key, &set_value, -1, std::to_underlying(StorageDeltaFlags::None));
    if (!res.has_value()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (*res != SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
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
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}
// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(NNIntAddTest, NNIntAddTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
