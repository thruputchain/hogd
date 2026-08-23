// A transaction that observes an RCQ entry and then deterministically faults is
// still included in the block: its body effects are discarded, but its RCQ
// observation (and replay nonce) must still be committed so that block-close
// matching can reject a forged observation.
//
// This closes a gap in the fault path. It previously called abort_transaction()
// on a deterministic fault, so a faulting transaction's observation deltas
// never reached HogDb and a forged observation on a faulting tx could not be
// rejected.
//
// The two tests below pin both directions:
//   - an honest observation on a faulting tx reconciles and the block closes;
//   - a forged observation (a value never inserted in the block) on a faulting
//     tx is rejected at block close.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

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
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class RcqObservationOnFaultTest : public ::testing::Test,
                                  public NativeContractTestFramework {
public:
  RcqObservationOnFaultTest()
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

  static constexpr ContractAddress kAddr = make_contract_address(0);

  // method 0: insert each calldata byte into rcq0.
  // method 1: peek-observe one entry from rcq0, then deterministically fault.
  static std::unique_ptr<NativeContext> make_ctx()
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto const &cd = ctx.handler().get_calldata();
          auto key = make_storage_key("rcq0");
          for (uint8_t val : cd) {
            auto res = ctx.handler().retrocausal_queue_insert(
                key, std::as_bytes(std::span(&val, 1)))();
            if (!res.has_value()) {
              co_return res.error().to_invoke_error();
            }
          }
          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto key = make_storage_key("rcq0");
          // Peek records an observation of a concurrent entry...
          auto peek = ctx.handler().retrocausal_queue_consume(key,
                                                              /*peek=*/true)();
          if (!peek.has_value()) {
            co_return peek.error().to_invoke_error();
          }
          // ...and then the transaction deterministically faults.
          co_return InvokeError{InvokeErrorCode::Deterministic};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

  static Transaction make_tx(uint32_t method, uint64_t nonce,
                             std::vector<uint8_t> const &calldata = {})
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddr,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(calldata.begin(), calldata.end())},
        .uniquifier =
            Uniquifier{.block_expiry_height = INT64_MAX, .nonce = nonce}};
  }
};

// An honest observation on a faulting transaction reconciles: the inserted
// value {10} matches the observed value, so the block closes successfully.
TEST_F(RcqObservationOnFaultTest, HonestObservationOnFaultClosesBlock)
{
  auto ctx = make_ctx();

  auto tx_insert = make_tx(0, /*nonce=*/1, {10});
  auto tx_observe_fault = make_tx(1, /*nonce=*/2);

  NondeterministicProposalData insert_p;
  NondeterministicProposalData observe_p;
  {
    auto h_ins = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(h_ins.root_invoke()().is_root_success());
    insert_p = h_ins.get_proposal_data();
    ASSERT_EQ(h_ins.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h_obs = make_new_system_call_handler(tx_observe_fault, *ctx);
    ASSERT_FALSE(h_obs.root_invoke()().is_root_success());
    observe_p = h_obs.get_proposal_data();
    // The faulting tx is still included (deterministic error), having recorded
    // its observation.
    ASSERT_EQ(h_obs.try_commit_transaction()(),
              ExecResult::ExecResultTxDeterministicError);
  }

  ASSERT_EQ(observe_p.observed_queue_entry_sequence.size(), 1U);
  ASSERT_EQ(observe_p.observed_queue_entry_sequence[0], QueueEntry{10});

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  {
    auto h_ins =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(h_ins.root_invoke()().is_root_success());
    ASSERT_EQ(h_ins.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h_obs = make_new_system_call_handler_validation(tx_observe_fault, *ctx,
                                                         observe_p);
    ASSERT_FALSE(h_obs.root_invoke()().is_root_success());
    ASSERT_EQ(h_obs.try_commit_transaction()(),
              ExecResult::ExecResultTxDeterministicError);
  }

  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation));
}

// A forged observation (a value never inserted in the block) on a faulting
// transaction must be rejected at block close. Before the fault path committed
// its observation deltas, the forged observation never reached HogDb and the
// block would close, accepting it.
TEST_F(RcqObservationOnFaultTest, ForgedObservationOnFaultRejectedAtBlockClose)
{
  auto ctx = make_ctx();

  auto tx_insert = make_tx(0, /*nonce=*/1, {10});
  auto tx_observe_fault = make_tx(1, /*nonce=*/2);

  NondeterministicProposalData insert_p;
  NondeterministicProposalData observe_p;
  {
    auto h_ins = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(h_ins.root_invoke()().is_root_success());
    insert_p = h_ins.get_proposal_data();
    ASSERT_EQ(h_ins.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h_obs = make_new_system_call_handler(tx_observe_fault, *ctx);
    ASSERT_FALSE(h_obs.root_invoke()().is_root_success());
    observe_p = h_obs.get_proposal_data();
    ASSERT_EQ(h_obs.try_commit_transaction()(),
              ExecResult::ExecResultTxDeterministicError);
  }

  ASSERT_EQ(observe_p.observed_queue_entry_sequence.size(), 1U);

  // Forge the observation to a value that is never inserted in this block.
  QueueEntry forged;
  forged.push_back(99);
  observe_p.observed_queue_entry_sequence[0] = forged;

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
  {
    auto h_ins =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(h_ins.root_invoke()().is_root_success());
    ASSERT_EQ(h_ins.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h_obs = make_new_system_call_handler_validation(tx_observe_fault, *ctx,
                                                         observe_p);
    ASSERT_FALSE(h_obs.root_invoke()().is_root_success());
    // The forged observation delta reaches HogDb on the fault path; it cannot
    // be reconciled ({99} was never inserted), so block close rejects it.
    ASSERT_EQ(h_obs.try_commit_transaction()(),
              ExecResult::ExecResultTxDeterministicError);
  }

  reject_block(BlockExecutionMode::kBlockExecutionModeValidation);
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
