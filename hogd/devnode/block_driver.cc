#include "hogd/devnode/block_driver.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <stop_token>

#include "consensus.hh"
#include "header.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "proposal.hh"
#include "proposal_hash.h"
#include "sched_coord.hh"
#include "transaction.hh"

namespace hogd {
namespace {
void
require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

// TimePoint is Unix nanoseconds; system_clock's epoch is the Unix epoch on
// every platform this builds for. Never behind [parent]: the scheduler refuses
// a block older than the committed tip, which a wall-clock step back would
// otherwise produce.
TimePoint
next_block_timepoint(TimePoint parent)
{
  const auto now = static_cast<TimePoint>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  return std::max(now, parent);
}
} // namespace

std::size_t
BlockDriver::produce_block()
{
  auto tip = scheduler_.schedcoord_get_tip()();
  require(tip.outcome() == SchedulerGetTipOutcome::SchedulerGetTipOK,
          "cannot read chain tip");
  auto proposal = scheduler_.schedulercoord_propose_block(
      tip.db_tip().id,
      ProposalContext{
          .block_timepoint = next_block_timepoint(
              tip.db_tip().value.proposal_header.block_timepoint)})();
  require(proposal.outcome() == SchedulerProposalOutcome::SchedulerProposalOK,
          "block proposal failed");
  const auto &block = proposal.proposed_block();
  SignedTransactionBatch txs;
  require(parallel_batch_to_t(block.txs, txs, kNoBatchSizeLimit)(),
          "cannot decode proposed transactions");
  ConsensusValue value{};
  value.prev_hash = sha256(tip.db_tip().value);
  value.proposal_header = block.proposal.header;
  value.proposal_txs_hash =
      proposal_txs_compatibility_hash(block.proposal.transactions);
  require(scheduler_.schedulercoord_commit_block(block.block_id, value)() ==
              SchedulerCommitOutcome::SchedulerCommitOK,
          "block commit failed");
  ++blocks_;
  transactions_ += txs.size();
  return txs.size();
}

void
BlockDriver::run(std::chrono::steady_clock::time_point epoch,
                 std::chrono::milliseconds interval,
                 const std::stop_token &stop)
{
  require(interval.count() > 0, "block interval must be positive");
  auto tick = epoch + interval;
  std::mutex mutex;
  std::unique_lock lock(mutex);
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    wake.wait_until(lock, stop, tick, [] { return false; });
    if (stop.stop_requested())
      break;
    produce_block();
    // Keep the requested cadence; skip missed ticks without bursts.
    tick += interval;
    const auto now = std::chrono::steady_clock::now();
    if (tick <= now)
      tick += interval * ((now - tick) / interval + 1);
  }
}

} // namespace hogd
