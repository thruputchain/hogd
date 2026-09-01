#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>

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
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

using SyscallTraceVec = x23::Vec<SyscallTrace, 0xffffffff>;

HostFunctionStatusXdr
status_with(SyscallErrorCode code)
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

} // namespace

// Trace pin for the tolerated RCQ peek anomaly (issue #1653): a peek that
// finds the current-block queue empty, followed by a consume that returns an
// entry a sibling committed in between.  The trace records the two reads as
// two independent syscalls -- an errored peek with no outputs, then a
// successful consume carrying the sibling's entry -- and the validation
// replay of the same transaction produces a byte-identical trace.  See
// anomalies/rcq_peek_head_after_sibling_commit_test.cc for why this is WAI.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class RcqPeekHeadTraceTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  RcqPeekHeadTraceTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  void SetUp() override
  {
    auto contract = make_native_contract(0);
    write_contract(contract);
    write_deployment(kAddr, contract);
    setup_grant_storage_token(kAddr, 1'000'000'000);
    peek_done_ = false;
    sibling_committed_ = false;
  }

protected:
  constexpr static GasAmount kGasLimit = 100'000;
  constexpr static ContractAddress kAddr = make_contract_address(0);
  constexpr static StorageKeyContractData kQueueKey =
      make_storage_key("trace_rcq_peek_head");
  constexpr static uint8_t kSiblingEntry = 0x4D;

  // Handshake between the peeking transaction (on its own thread) and the
  // test body.  Not reset between the proposal run and the validation replay,
  // so the replayed contract passes straight through the park.
  inline static std::atomic<bool> peek_done_ = false;
  inline static std::atomic<bool> sibling_committed_ = false;

  // method 0: insert kSiblingEntry at kQueueKey.
  // method 1: peek kQueueKey (must be empty), park until the sibling has
  //           committed, then destructively consume kQueueKey (must return
  //           kSiblingEntry).
  static std::unique_ptr<NativeContext> make_context()
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  std::array<uint8_t, 1> entry = {kSiblingEntry};
                  auto ins = ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
                      kQueueKey, std::as_bytes(std::span(entry)));
                  if (!ins.has_value() ||
                      ins.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
    fns.emplace(
        1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          std::array<std::byte, 1> buffer{};
          RuntimeMemoryLength written_len = 0;
          auto peek = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kQueueKey, true, std::span<std::byte>(buffer), written_len);

          // Signal unconditionally so the test body never deadlocks on a failed
          // peek; the trace assertions run after the join.
          peek_done_ = true;
          peek_done_.notify_all();
          sibling_committed_.wait(false);

          if (!peek.has_value() ||
              peek.value() !=
                  SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry ||
              written_len != 0) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto consume = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kQueueKey, false, std::span<std::byte>(buffer), written_len);
          if (!consume.has_value() ||
              consume.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              written_len != 1 || buffer[0] != std::byte{kSiblingEntry}) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

  Transaction make_tx(uint32_t method)
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = kAddr,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = kGasLimit,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  }

  static SyscallTraceVec expected_trace()
  {
    SyscallTraceVec expected;
    {
      // The empty peek: the input is recorded, no outputs are written, and
      // the status carries the no-entry error.
      SyscallTraceBody body(SyscallNo::kRetrocausalQueueConsume);
      body.retrocausal_queue_consume().inputs.key = kQueueKey;
      body.retrocausal_queue_consume().inputs.peek = true;
      expected.push_back(make_entry(
          kGasLimit, kGasLimit, std::move(body),
          status_with(
              SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry)));
    }
    {
      // The consume that follows returns the sibling's entry.
      QueueEntry entry{kSiblingEntry};
      SyscallTraceBody body(SyscallNo::kRetrocausalQueueConsume);
      body.retrocausal_queue_consume().inputs.key = kQueueKey;
      body.retrocausal_queue_consume().inputs.peek = false;
      body.retrocausal_queue_consume().outputs =
          SyscallRetrocausalQueueConsumeOutputs{.queue_entry = entry};
      expected.push_back(
          make_entry(kGasLimit, kGasLimit, std::move(body),
                     status_with(SyscallErrorCode::kSyscallErrorCodeNone)));
    }
    return expected;
  }
};

TEST_F(RcqPeekHeadTraceTest, EmptyPeekThenConsumeOfSiblingEntryTraceIsExact)
{
  auto context = make_context();
  auto tx_insert = make_tx(0);
  auto tx_peek_consume = make_tx(1);
  auto expected = expected_trace();

  // Proposal: TX_B peeks (empty) on a worker thread and parks; TX_A inserts
  // and commits on this thread; TX_B resumes and consumes.
  bool root_ok = false;
  bool trace_ok = false;
  std::optional<ExecResult> commit;
  NondeterministicProposalData peek_consume_p;
  std::thread worker([&] {
    auto handler = make_new_system_call_handler(tx_peek_consume, *context);
    handler.enable_tracing();
    root_ok = handler.root_invoke().is_root_success();
    peek_consume_p = handler.get_proposal_data();
    if (handler.get_trace() != nullptr) {
      trace_ok = handler.get_trace()->system_calls == expected;
      if (!trace_ok) {
        ADD_FAILURE() << "proposal trace: "
                      << x23::show(handler.get_trace()->system_calls);
      }
    }
    if (root_ok) {
      commit = handler.try_commit_transaction();
    }
    else {
      EXPECT_TRUE(handler.abort_transaction().has_value());
    }
  });

  peek_done_.wait(false);
  NondeterministicProposalData insert_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *context);
    EXPECT_TRUE(ha.root_invoke().is_root_success());
    insert_p = ha.get_proposal_data();
    EXPECT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);
  }
  sibling_committed_ = true;
  sibling_committed_.notify_all();
  worker.join();

  ASSERT_TRUE(root_ok);
  ASSERT_TRUE(trace_ok)
      << "known anomaly: the trace records an empty peek followed by a "
         "consume of the sibling's entry (issue #1653)";
  ASSERT_EQ(commit, ExecResult::ExecResultOK);

  auto proposal_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: replaying TX_B from its proposal data reproduces the exact
  // same trace with no interleaving at all.
  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *context, insert_p);
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_peek_consume, *context,
                                                std::move(peek_consume_p));
    hb.enable_tracing();
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_NE(hb.get_trace(), nullptr);
    ASSERT_EQ(hb.get_trace()->system_calls, expected)
        << x23::show(hb.get_trace()->system_calls);
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  std::optional<StorageShortHash> validation_hash;
  ASSERT_NO_THROW(validation_hash = advance_block(
                      BlockExecutionMode::kBlockExecutionModeValidation));
  EXPECT_TRUE(proposal_hash == *validation_hash);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
