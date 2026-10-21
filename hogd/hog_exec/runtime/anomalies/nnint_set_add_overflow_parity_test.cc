// Tolerated anomaly: an NNInt add that would overflow INT128_MAX passes the
// syscall and fails at commit as ExecResultTxNonDeterministicError, the same
// way whether the base was set explicitly or taken from the snapshot.
//
// See:
//   - https://github.com/groundhogchain/hogd/issues/1634
//   - https://github.com/groundhogchain/hogd/issues/2155

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/network_config_params.h"
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

  // The set value explicit-mode adds pass; the result the contract last saw.
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
          auto res =
              co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(key, set,
                                                                     add);
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
                       .gas_limit = kDefaultMaxTxGas,
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
      return std::ranges::any_of(conflict.conflicts(), [](auto const &c) {
        return c.code == DbConflictCode::Overflow;
      });
    }
    else {
      return conflict.level() == DbDiagnosticLevel::kDbDiagnosticFirst &&
             conflict.first().code == DbConflictCode::Overflow;
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

// Issue #1634: adding 1 to a committed INT128_MAX fails identically in both
// modes.
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

// Boundary control: adding up to exactly INT128_MAX commits in both modes.
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
