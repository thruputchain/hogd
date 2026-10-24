// Tolerated concurrency anomaly: an RCQ peek does not reserve the queue head
// against sibling commits.
//
// A transaction that peeks the current-block queue, sees it empty, and later
// consumes from the same queue can receive an entry that a sibling
// transaction committed in between.  The peek neither advances nor reserves
// the concurrent-entry cursor, and an absent concurrent entry is not cached,
// so the later consume re-queries the same index and finds the newly
// committed entry.
//
// Why this is tolerated:
//   - Current-block entries are concurrent by definition.  Sibling
//     transactions commit independently on worker threads, so which entries
//     are visible at an index is a function of the proposer's schedule.  A
//     different proposer could have committed the sibling before the peek,
//     making the consume result the honest head from the start.
//   - Each concurrent-entry read is logged as its own nondeterministic
//     observation (bit=false for the empty peek, bit=true plus the entry for
//     the consume).  Validators replay exactly what the proposer saw, so the
//     block closes identically on every node.  There is no consensus
//     divergence.
//   - Reserving the head at peek time, or caching absence, would turn a
//     read-only peek into a claim on queue state without changing what an
//     adversarial proposer can already make a contract observe (see the
//     value-based reconciliation pins in
//     rcq_observation_by_value_not_index_test.cc).
//   - Peek stability does hold within the transaction's own view: past-block
//     entries and self-inserts are consumed ahead of concurrent entries, and a
//     concurrent entry that a peek did observe is cached for the block, so a
//     peeked entry is the entry consumed next.  The second test pins that
//     boundary.
//
// Contracts must not treat an empty peek as a promise that a later consume in
// the same transaction is also empty.
//
// See:
//   - https://github.com/thruputchain/hogd/issues/1653

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "retrocausal_queue.hh"
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
class RcqPeekHeadAfterSiblingCommitTest : public ::testing::Test,
                                          public NativeContractTestFramework {
public:
  RcqPeekHeadAfterSiblingCommitTest()
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
    peek_done_ = false;
    sibling_committed_ = false;
  }

  static constexpr ContractAddress kAddr = make_contract_address(0);
  static constexpr StorageKeyContractData kQueueKey = make_storage_key("rcq0");

  // Handshake between the peeking transaction (on its own thread) and the
  // test body: the contract signals peek_done_ after its peek and parks on
  // sibling_committed_ until the test has committed the sibling.  The park
  // happens on a thread outside the coroutine pool, because the contract
  // frame resumes on a pool worker after the peek (see method 1).  The flags
  // are deliberately not reset between the proposal run and the validation
  // replay, so the replayed contract passes straight through the park.
  inline static std::atomic<bool> peek_done_ = false;
  inline static std::atomic<bool> sibling_committed_ = false;

  // What the peek-then-consume contract observed on its most recent run.
  std::optional<SyscallErrorCode> peek_error_;
  std::optional<std::vector<uint8_t>> peek_entry_;
  std::optional<std::vector<uint8_t>> consumed_entry_;

  void clear_observations()
  {
    peek_error_.reset();
    peek_entry_.reset();
    consumed_entry_.reset();
  }

  static std::vector<uint8_t> to_bytes(QueueEntry const &entry)
  {
    return {entry.data(), entry.data() + entry.size()};
  }

  // method 0: insert each calldata byte as its own entry at kQueueKey.
  // method 1: peek kQueueKey, park until the sibling has committed, then
  //           destructively consume kQueueKey and persist the entry under
  //           "result".  The contract requires the consume to return an entry.
  std::unique_ptr<NativeContext> make_context()
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    map.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto const &cd = ctx.handler().get_calldata();
          for (uint8_t val : cd) {
            auto res = co_await ctx.syscall<SyscallNo::kRetrocausalQueueInsert>(
                kQueueKey, std::as_bytes(std::span(&val, 1)));
            if (!res.has_value()) {
              co_return NativeContractBuilder::host_fn_error_conversion(
                  res.error());
            }
          }
          co_return InvokeStatus<void>{};
        });
    map.emplace(
        1, [this](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          QueueEntry peeked;
          peeked.resize(kQueueEntryMaxLen);
          RuntimeMemoryLength peeked_len = 0;
          auto peek = co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              kQueueKey, /*peek=*/true,
              std::as_writable_bytes(std::span(peeked)), peeked_len);
          if (peek.has_value()) {
            if (peek.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
              peeked.resize(peeked_len);
              peek_entry_ = to_bytes(peeked);
            }
            else {
              peek_error_ = peek.value();
            }
          }

          // Signal unconditionally so the test body never deadlocks on a
          // failed peek; the assertions run after the join.
          peek_done_ = true;
          peek_done_.notify_all();

          // Park on a thread outside the pool.  The peek above resumes this
          // body by symmetric transfer straight from the RPC reply, so the
          // frame is running on the thread pool worker that drains that
          // stream.  Blocking that worker here stalls the transport, so the
          // sibling transaction the test body runs never completes and never
          // sets the flag -- a deadlock, not just a parked worker.  (Same
          // park as the concurrent-insert case in rcq_delete_test.cc.)
          co_await default_coropoll().pool_.async(
              [] { sibling_committed_.wait(false); });

          if (!peek.has_value()) {
            co_return NativeContractBuilder::host_fn_error_conversion(
                peek.error());
          }

          QueueEntry entry;
          entry.resize(kQueueEntryMaxLen);
          RuntimeMemoryLength entry_len = 0;
          auto consume =
              co_await ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
                  kQueueKey, /*peek=*/false,
                  std::as_writable_bytes(std::span(entry)), entry_len);
          if (!consume.has_value()) {
            co_return NativeContractBuilder::host_fn_error_conversion(
                consume.error());
          }
          if (consume.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          entry.resize(entry_len);
          consumed_entry_ = to_bytes(entry);

          auto ws = co_await ctx.syscall<SyscallNo::kBytestringSet>(
              make_storage_key("result"),
              std::as_bytes(std::span(entry.data(), entry.size())));
          if (!ws.has_value() ||
              ws.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
    cmap.emplace(0, std::make_unique<NativeContract>(std::move(map)));
    return std::make_unique<NativeContext>(std::move(cmap));
  }

  Transaction make_tx(uint32_t method,
                      std::vector<uint8_t> const &calldata = {})
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddr,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = kDefaultMaxTxGas,
                       .calldata = Calldata(calldata.begin(), calldata.end())},
        .uniquifier = make_new_uniquifier()};
  }

  struct ProposalRun {
    bool root_ok = false;
    std::optional<ExecResult> commit;
    NondeterministicProposalData proposal_data;
  };

  // Runs the peek-then-consume tx on a worker thread, and runs `sibling` on
  // this thread while that tx is parked between its peek and its consume.
  // Only EXPECT-style checks may run inside `sibling`: an early return would
  // leave the worker parked and un-joined.
  ProposalRun run_peek_consume_interleaved(Transaction const &tx,
                                           NativeContext &ctx,
                                           std::function<void()> const &sibling)
  {
    ProposalRun run;
    std::thread worker([&] {
      auto handler = make_new_system_call_handler(tx, ctx);
      run.root_ok = handler.root_invoke()().is_root_success();
      run.proposal_data = handler.extract_proposal_data();
      if (run.root_ok) {
        run.commit = handler.try_commit_transaction()();
      }
      else {
        EXPECT_TRUE(handler.abort_transaction()().has_value());
      }
    });

    peek_done_.wait(false);
    sibling();
    sibling_committed_ = true;
    sibling_committed_.notify_all();
    worker.join();
    return run;
  }

  static bool nondeterministic_bit(NondeterministicProposalData const &p,
                                   uint32_t index)
  {
    return ((p.nondeterministic_bits[index / 8] >> (index % 8)) & 1U) != 0;
  }
};

// Issue #1653: an empty peek followed by a consume that returns a sibling's
// entry.
//
// TX_B peeks an empty queue and sees no entry.  While TX_B is parked, TX_A
// inserts {77} into that queue and commits.  TX_B then destructively consumes
// the same queue and receives {77}.
//
// WAI: the two reads are independent nondeterministic observations (one
// empty, one {77}), validation replays both from the proposal data, and the
// block closes with identical state on the proposer and the validator.
TEST_F(RcqPeekHeadAfterSiblingCommitTest,
       EmptyPeekThenConsumeReturnsSiblingEntryCommittedInBetween)
{
  auto ctx = make_context();
  auto tx_insert = make_tx(0, {77});
  auto tx_peek_consume = make_tx(1);

  NondeterministicProposalData insert_p;
  auto run = run_peek_consume_interleaved(tx_peek_consume, *ctx, [&] {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    EXPECT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.extract_proposal_data();
    EXPECT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  });

  ASSERT_TRUE(run.root_ok);
  ASSERT_EQ(run.commit, ExecResult::ExecResultOK);

  // The peek saw nothing; the consume saw the sibling's entry.
  ASSERT_FALSE(peek_entry_.has_value());
  ASSERT_EQ(peek_error_,
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry);
  ASSERT_EQ(consumed_entry_, (std::vector<uint8_t>{77}))
      << "known anomaly: a consume after an empty peek returns the entry a "
         "sibling committed in between (issue #1653)";

  // Both reads were recorded independently: an absent observation for the
  // peek, then a present observation carrying the consumed entry.
  auto const &peek_p = run.proposal_data;
  ASSERT_EQ(peek_p.nondeterministic_bit_count, 2U);
  EXPECT_FALSE(nondeterministic_bit(peek_p, 0));
  EXPECT_TRUE(nondeterministic_bit(peek_p, 1));
  ASSERT_EQ(peek_p.observed_queue_entry_sequence.size(), 1U);
  EXPECT_EQ(peek_p.observed_queue_entry_sequence[0], QueueEntry{77});

  auto proposal_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation replays the recorded observations, so the validator's TX_B
  // sees the same empty peek and the same {77} consume even though no
  // interleaving happens here.
  clear_observations();
  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_peek_consume, *ctx, peek_p);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  EXPECT_FALSE(peek_entry_.has_value());
  EXPECT_EQ(peek_error_,
            SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry);
  EXPECT_EQ(consumed_entry_, (std::vector<uint8_t>{77}));

  std::optional<StorageShortHash> validation_hash;
  ASSERT_NO_THROW(validation_hash = advance_block(
                      BlockExecutionMode::kBlockExecutionModeValidation))
      << "value-based reconciliation accepts the consume because {77} was "
         "inserted by a committing sibling in the same block";
  EXPECT_TRUE(proposal_hash == *validation_hash)
      << "proposal and validation must commit identical state for the "
         "empty-peek-then-consume sequence (issue #1653)";

  auto result =
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "result"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->bytestring(), (Bytestring{77}));
}

// The boundary of the anomaly: a concurrent entry that the peek did observe
// stays the head.
//
// TX_A0 inserts {10} and commits before TX_B starts.  TX_B peeks and sees
// {10}.  While TX_B is parked, TX_A1 inserts {20} and commits.  TX_B then
// consumes and still receives {10}: the observed entry is pinned at its index
// for the rest of the block, and the re-observation is not logged twice.
TEST_F(RcqPeekHeadAfterSiblingCommitTest,
       PeekedConcurrentEntryStaysHeadAfterSiblingCommit)
{
  auto ctx = make_context();
  auto tx_insert_10 = make_tx(0, {10});
  auto tx_insert_20 = make_tx(0, {20});
  auto tx_peek_consume = make_tx(1);

  NondeterministicProposalData insert_10_p;
  {
    auto ha = make_new_system_call_handler(tx_insert_10, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_10_p = ha.extract_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  NondeterministicProposalData insert_20_p;
  auto run = run_peek_consume_interleaved(tx_peek_consume, *ctx, [&] {
    auto ha = make_new_system_call_handler(tx_insert_20, *ctx);
    EXPECT_TRUE(ha.root_invoke()().is_root_success());
    insert_20_p = ha.extract_proposal_data();
    EXPECT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);
  });

  ASSERT_TRUE(run.root_ok);
  ASSERT_EQ(run.commit, ExecResult::ExecResultOK);

  ASSERT_FALSE(peek_error_.has_value());
  EXPECT_EQ(peek_entry_, (std::vector<uint8_t>{10}));
  EXPECT_EQ(consumed_entry_, (std::vector<uint8_t>{10}))
      << "a concurrent entry observed by a peek must remain the entry a "
         "later consume returns, even after a sibling commits";

  // Two present reads of the same (queue, index) record two bits but a single
  // observation.
  auto const &peek_p = run.proposal_data;
  ASSERT_EQ(peek_p.nondeterministic_bit_count, 2U);
  EXPECT_TRUE(nondeterministic_bit(peek_p, 0));
  EXPECT_TRUE(nondeterministic_bit(peek_p, 1));
  ASSERT_EQ(peek_p.observed_queue_entry_sequence.size(), 1U);
  EXPECT_EQ(peek_p.observed_queue_entry_sequence[0], QueueEntry{10});

  auto proposal_hash =
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  clear_observations();
  {
    auto h10 = make_new_system_call_handler_validation(tx_insert_10, *ctx,
                                                       insert_10_p);
    ASSERT_TRUE(h10.root_invoke()().is_root_success());
    ASSERT_EQ(h10.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h20 = make_new_system_call_handler_validation(tx_insert_20, *ctx,
                                                       insert_20_p);
    ASSERT_TRUE(h20.root_invoke()().is_root_success());
    ASSERT_EQ(h20.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_peek_consume, *ctx, peek_p);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }
  EXPECT_EQ(peek_entry_, (std::vector<uint8_t>{10}));
  EXPECT_EQ(consumed_entry_, (std::vector<uint8_t>{10}));

  std::optional<StorageShortHash> validation_hash;
  ASSERT_NO_THROW(validation_hash = advance_block(
                      BlockExecutionMode::kBlockExecutionModeValidation));
  EXPECT_TRUE(proposal_hash == *validation_hash);

  auto result =
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "result"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->bytestring(), (Bytestring{10}));
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
