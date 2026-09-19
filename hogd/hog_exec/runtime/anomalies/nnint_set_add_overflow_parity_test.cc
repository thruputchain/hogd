// Tolerated anomaly: an NNInt add that would overflow INT128_MAX is not
// rejected at the syscall; it fails at commit, and it fails the same way
// whether the base was set explicitly or taken from the snapshot.
//
// NonnegativeIntSetAdd has two modes.  With an explicit set_value the
// executor stages set_add(set_value, add_value); without one it stages
// add(add_value) and the base is whatever the database snapshot holds at
// commit.  In the implicit mode the executor cannot see the base, so an
// overflowing add can only be caught by hog_db at reservation.  In the
// explicit mode the executor could check base + add locally, but it does
// not: NNIntItem::validate() accepts every nonnegative add_value (the
// protocol invariant is value >= min(set_value, 0), see #2155), so an
// explicit set_add(INT128_MAX, +1) is staged and also caught by hog_db.
//
// Either way the transaction is excluded from the block as
// ExecResultTxNonDeterministicError, with a kDbConflictOverflow diagnostic,
// instead of being included as a deterministic InvalidSyscallInput fault.
// Issue #1634 reported the explicit case as a misclassification.
//
// Why this is tolerated:
//   - The two modes agree.  A contract that adds to a high NNInt observes the
//     same syscall result (kSyscallErrorCodeNone), the same commit outcome and
//     the same diagnostic whether or not it passed the base explicitly.  A
//     syscall-time check in the explicit mode alone would split them: the
//     explicit overflow would be included in the block as a deterministic
//     fault while the implicit one stayed excluded.
//   - The transaction only harms itself.  It is dropped from the proposal,
//     the key keeps its committed value, and the contract can resubmit with
//     a representable add.
//   - No consensus divergence.  An excluded transaction is never replayed by
//     validators, and hog_db rejects the overflow at reservation, so nothing
//     impossible is ever staged.
//   - The failure is attributable: since #2878 the commit diagnostic names
//     the key and kDbConflictOverflow.
//
// Contracts must not rely on NonnegativeIntSetAdd returning
// InvalidSyscallInput for an overflowing explicit set-add.
//
// See:
//   - https://github.com/groundhogchain/hogd/issues/1634
//   - https://github.com/groundhogchain/hogd/issues/2155

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "exec_result.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
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
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class NNIntSetAddOverflowParityTest : public ::testing::Test,
                                      public NativeContractTestFramework {
public:
  NNIntSetAddOverflowParityTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  void SetUp() override
  {
    auto contract_xdr = make_native_contract(0);
    write_contract(contract_xdr);
    write_deployment(kAddr, contract_xdr);
    setup_grant_storage_token(kAddr, 1'000'000'000);
  }

  static constexpr int128_t kMax = std::numeric_limits<int128_t>::max();
  static constexpr ContractAddress kAddr = make_contract_address(0);

  // One key per mode, so the explicit and implicit transactions in a block
  // never share a lock and their outcomes are independent.
  enum class Key : uint8_t { kExplicit = 0, kImplicit = 1 };
  static constexpr std::string_view key_name(Key key)
  {
    return key == Key::kExplicit ? "nnint_explicit" : "nnint_implicit";
  }

  enum class Mode : uint8_t { kExplicit = 0, kImplicit = 1 };

  // Calldata: [key, mode, add].  Explicit mode passes explicit_set_ as the
  // set value; implicit mode passes no set value and lets the snapshot supply
  // the base.  The contract records the syscall's result and faults
  // deterministically on anything but kSyscallErrorCodeNone.
  int128_t explicit_set_ = 0;
  std::optional<SyscallErrorCode> last_syscall_code_;

  std::unique_ptr<NativeContext> make_context()
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    map.emplace(
        0, [this](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto const &cd = ctx.handler().get_calldata();
          if (cd.size() != 3) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto key = make_storage_key(key_name(static_cast<Key>(cd[0])));
          auto mode = static_cast<Mode>(cd[1]);
          int128_t add = cd[2];
          int128_t const *set =
              mode == Mode::kExplicit ? &explicit_set_ : nullptr;
          auto res = co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
              key, set, add, std::to_underlying(StorageDeltaFlags::None));
          if (!res.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          last_syscall_code_ = *res;
          if (*res != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(map)));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

  Transaction make_tx(Key key, Mode mode, uint8_t add)
  {
    std::vector<uint8_t> const calldata{std::to_underlying(key),
                                        std::to_underlying(mode), add};
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddr,
                       .entrypoint = entrypoint_from_method_id(0),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(calldata.begin(), calldata.end())},
        .uniquifier = make_new_uniquifier()};
  }

  struct AddRun {
    std::optional<SyscallErrorCode> syscall_code;
    bool root_ok = false;
    ExecResult commit = ExecResult::ExecResultUnrecoverable;
    bool overflow_diagnosed = false;
  };

  // Runs one add through root_invoke and commit and reports what each stage
  // returned.
  AddRun run_add(NativeContext &ctx, Key key, Mode mode, uint8_t add)
  {
    last_syscall_code_.reset();
    auto handler = make_new_system_call_handler(make_tx(key, mode, add), ctx);
    AddRun run;
    run.root_ok = handler.root_invoke()().is_root_success();
    run.syscall_code = last_syscall_code_;
    if (run.root_ok) {
      run.commit = handler.try_commit_transaction()();
      run.overflow_diagnosed = has_overflow_conflict(handler);
    }
    else {
      EXPECT_TRUE(handler.abort_transaction()().has_value());
    }
    return run;
  }

  // Commits `value` to both keys through explicit set-adds and closes the
  // block, so the next block's snapshot holds `value` at each key.
  void seed_both_keys(NativeContext &ctx, int128_t value)
  {
    explicit_set_ = value;
    for (auto key : {Key::kExplicit, Key::kImplicit}) {
      ASSERT_EQ(run_add(ctx, key, Mode::kExplicit, 0).commit,
                ExecResult::ExecResultOK);
    }
    advance_block();
    for (auto key : {Key::kExplicit, Key::kImplicit}) {
      ASSERT_EQ(read_nnint(key), value);
    }
  }

  static bool has_overflow_conflict(SystemCallHandler const &handler)
  {
    auto const &conflict = handler.last_commit_conflict();
    // Proposal mode: hog_db selects All in debug builds, First in release.
    if constexpr (is_debug_build()) {
      if (conflict.level() != DbDiagnosticLevel::kDbDiagnosticAll) {
        return false;
      }
      for (auto const &c : conflict.conflicts()) {
        if (c.code == DbConflictCode::kDbConflictOverflow) {
          return true;
        }
      }
      return false;
    }
    else {
      return conflict.level() == DbDiagnosticLevel::kDbDiagnosticFirst &&
             conflict.first().code == DbConflictCode::kDbConflictOverflow;
    }
  }

  std::optional<int128_t> read_nnint(Key key)
  {
    auto item = lookup_storage_item_from_db(
        make_contract_data_addrkey(kAddr, key_name(key)));
    if (!item.has_value() ||
        item->type() != StorageItemType::kItemTypeNonnegativeInt) {
      return std::nullopt;
    }
    return from_xdr(item->nnint());
  }
};

// Issue #1634: add 1 to a committed INT128_MAX.
//
// Explicit mode stages set_add(INT128_MAX, +1); implicit mode stages add(+1)
// against the same committed base.  Both syscalls return
// kSyscallErrorCodeNone, both root invocations succeed, and both commits are
// rejected by hog_db as an overflow and surface as
// ExecResultTxNonDeterministicError.  Neither key changes.
TEST_F(NNIntSetAddOverflowParityTest,
       ExplicitAndImplicitOverflowFailIdenticallyAtCommit)
{
  auto ctx = make_context();
  ASSERT_NO_FATAL_FAILURE(seed_both_keys(*ctx, kMax));

  auto explicit_run = run_add(*ctx, Key::kExplicit, Mode::kExplicit, 1);
  auto implicit_run = run_add(*ctx, Key::kImplicit, Mode::kImplicit, 1);

  // The syscall accepts the overflowing add in both modes.
  EXPECT_EQ(explicit_run.syscall_code, SyscallErrorCode::kSyscallErrorCodeNone)
      << "known anomaly: an explicit set_add(INT128_MAX, +1) is not rejected "
         "as InvalidSyscallInput (issue #1634)";
  EXPECT_EQ(implicit_run.syscall_code, SyscallErrorCode::kSyscallErrorCodeNone);
  ASSERT_TRUE(explicit_run.root_ok);
  ASSERT_TRUE(implicit_run.root_ok);

  // Commit rejects both the same way, and names the overflow.
  EXPECT_EQ(explicit_run.commit, ExecResult::ExecResultTxNonDeterministicError);
  EXPECT_EQ(implicit_run.commit, ExecResult::ExecResultTxNonDeterministicError);
  EXPECT_TRUE(explicit_run.overflow_diagnosed);
  EXPECT_TRUE(implicit_run.overflow_diagnosed);

  // Both transactions were excluded: the keys keep their committed values.
  advance_block();
  EXPECT_EQ(read_nnint(Key::kExplicit), kMax);
  EXPECT_EQ(read_nnint(Key::kImplicit), kMax);
}

// The boundary: adding up to exactly INT128_MAX is accepted in both modes.
// The modes diverge from each other nowhere; they only diverge from a
// deterministic fault once the sum is unrepresentable.
TEST_F(NNIntSetAddOverflowParityTest,
       ExplicitAndImplicitAddToBoundarySucceedIdentically)
{
  auto ctx = make_context();
  ASSERT_NO_FATAL_FAILURE(seed_both_keys(*ctx, kMax - 1));

  auto explicit_run = run_add(*ctx, Key::kExplicit, Mode::kExplicit, 1);
  auto implicit_run = run_add(*ctx, Key::kImplicit, Mode::kImplicit, 1);

  EXPECT_EQ(explicit_run.syscall_code, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(implicit_run.syscall_code, SyscallErrorCode::kSyscallErrorCodeNone);
  ASSERT_TRUE(explicit_run.root_ok);
  ASSERT_TRUE(implicit_run.root_ok);
  EXPECT_EQ(explicit_run.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(implicit_run.commit, ExecResult::ExecResultOK);
  EXPECT_FALSE(explicit_run.overflow_diagnosed);
  EXPECT_FALSE(implicit_run.overflow_diagnosed);

  advance_block();
  EXPECT_EQ(read_nnint(Key::kExplicit), kMax);
  EXPECT_EQ(read_nnint(Key::kImplicit), kMax);
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
