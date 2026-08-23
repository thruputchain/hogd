#include <cstdint>
#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <utility>

#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"

namespace hogd {

// Verifies the executor forwards a best-effort TxFailureDiagnostic to the
// event server when (and only when) hog_db attaches conflict diagnostics to a
// failed 2-phase-commit push. Parameterized over the db test double: real
// hog_db produces diagnostics for proposal blocks; the mock produces none, so
// no report may be sent.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class TxFailureReportTest : public ::testing::TestWithParam<TestDB>,
                            public NativeContractTestFramework {
public:
  TxFailureReportTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

TEST_P(TxFailureReportTest, NonDeterministicFailureForwardsOneReport)
{
  // Conflict recipe adapted from NNIntAddTest.SetAddWithAdd: method 0
  // set_adds (0, 200); method 1 adds -100. NNIntAddTest runs both methods
  // back-to-back within one block, which the mock db test double permits
  // (it does not model per-block lock capacity). Real hog_db's two-phase
  // commit binds a key's subtraction capacity for the whole block to the
  // item's value as of the block's start (so same-block ops compose
  // order-independently regardless of push order); a set_add that creates
  // an item within a block therefore cannot be netted against by an add in
  // that same block. So here method 0 and method 1 each get their own
  // block for the first (non-conflicting) pair. The second pair keeps
  // NNIntAddTest's original same-block shape: method 0's set_add resets the
  // block's capacity to base 0, so method 1's add -100 fails the push --
  // still a genuine hog_db 2pc conflict (kDbConflictBoundsExceeded rather
  // than the value-mismatch NNIntAddTest's comment describes, but exactly
  // the failed-push diagnostic path this test exists to exercise).
  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, // set_add on (0, 200)
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
    fns.emplace(1, // add on (-100)
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
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto run_method = [&](Runtime::MethodID method) {
    return make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_addr,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier(),
                }},
        *context);
  };

  // Block 1: method 0 creates the item (set_add(0, 200)).
  auto handler1 = run_method(0);
  ASSERT_TRUE(handler1.root_invoke()().is_root_success());
  ASSERT_EQ(handler1.try_commit_transaction()(), ExecResult::ExecResultOK);
  advance_block();

  // Block 2: method 1 nets the item down to 100 in its own block, so its
  // subtraction is bounded by the item's already-committed value.
  auto handler2 = run_method(1);
  ASSERT_TRUE(handler2.root_invoke()().is_root_success());
  ASSERT_EQ(handler2.try_commit_transaction()(), ExecResult::ExecResultOK);
  advance_block();

  // Block 3: method 0's set_add resets this block's capacity to base 0, so
  // method 1's push fails the 2-phase commit.
  auto handler3 = run_method(0);
  ASSERT_TRUE(handler3.root_invoke()().is_root_success());
  ASSERT_EQ(handler3.try_commit_transaction()(), ExecResult::ExecResultOK);
  auto handler4 = run_method(1);
  ASSERT_TRUE(handler4.root_invoke()().is_root_success());
  ASSERT_EQ(handler4.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError);

  if (GetParam() == TestDB::kHogDb) {
    // hog_db attached diagnostics; exactly one best-effort report must reach
    // the event server, carrying this tx's hash and the executing height.
    wait_for_failure_reports(1)();
    auto reports = get_failure_reports()();
    ASSERT_EQ(reports.size(), 1);
    EXPECT_EQ(reports[0].first, handler4.get_source_tx_hash());
    EXPECT_GE(reports[0].second.attempt_height, 1);
  }
  else {
    // The mock db test double produces no diagnostics (level None), so the
    // executor must not send anything. No send is ever initiated in this
    // case (the level check is synchronous), so this read does not race.
    EXPECT_EQ(get_failure_reports()().size(), 0);
  }
}

INSTANTIATE_TEST_SUITE_P(TxFailureReportTest, TxFailureReportTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

} // namespace hogd
