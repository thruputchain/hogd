#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/event/event_block_context.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/pull_controller.h"
#include "hogd/indexer/recent_tx_failure_cache.h"
#include "hogd/version.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "network.hh"
#include "proposal_hash.h"
#include "x23/async.h"

namespace hogd {
namespace {

class CaughtUpPullRpc {
public:
  using rpc_service = EventIndexer1;

  CaughtUpPullRpc()
  {
    origin_.protocol = version::kHogdProtocol();
    origin_.first_height = IndexDB::kFirstOutboxBlockHeight;
    std::ranges::fill(origin_.first_prev_hash, 0x42);
    ProposalTxs proposal{};
    ConsensusValue value{};
    value.prev_hash = origin_.first_prev_hash;
    value.proposal_header.block_height = origin_.first_height;
    value.proposal_header.block_timepoint = 1000;
    value.proposal_header.events_hash =
        compute_indexer_outbox_event_root(TxEventLogBatch{});
    auto txs_root = compute_indexer_proposal_txs_root(proposal)();
    CHECK(txs_root.has_value());
    value.proposal_header.txs_hash = *txs_root;
    value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal);
    IndexerCommittedBlockPayload payload{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = std::move(value),
        .proposal_txs = std::move(proposal),
    };
    auto block = make_indexer_committed_block(payload)();
    CHECK(block.has_value());
    block_ = std::move(*block);
    std::ranges::fill(notice_.tx_hash, 0x51);
    notice_.diagnostic.attempt_height = 0;
    notice_.diagnostic.conflict.code = DbConflictCode::kDbConflictValueMismatch;
  }

  x23::Async<EventIndexerPullResult>
  event_indexer_pull_next(EventIndexerPullRequest request)
  {
    {
      std::lock_guard lock(mutex_);
      requests_.push_back(std::move(request));
    }
    const std::size_t call = calls_.fetch_add(1, std::memory_order_relaxed);
    if (call == 0) {
      co_return EventIndexerPullResult(
          EventIndexerPullOutcome::EventIndexerPullNotReady);
    }
    if (call == 1) {
      EventIndexerPullResult result(
          EventIndexerPullOutcome::EventIndexerPullBlock);
      result.pulled_block().origin = origin_;
      result.pulled_block().block = block_;
      co_return result;
    }
    EventIndexerPullResult result(
        EventIndexerPullOutcome::EventIndexerPullCaughtUp);
    result.origin() = origin_;
    co_return result;
  }

  x23::Async<IndexerTxFailureNoticeBatch> event_indexer_pull_tx_failures()
  {
    IndexerTxFailureNoticeBatch notices;
    if (!notice_returned_.exchange(true, std::memory_order_relaxed)) {
      notices.push_back(notice_);
    }
    co_return notices;
  }

  std::vector<EventIndexerPullRequest> requests() const
  {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  const IndexerOutboxOrigin &origin() const { return origin_; }
  const SignedTransactionHash &failure_hash() const { return notice_.tx_hash; }

private:
  IndexerOutboxOrigin origin_{};
  IndexerCommittedBlock block_{};
  IndexerTxFailureNotice notice_{};
  std::atomic<std::size_t> calls_ = 0;
  std::atomic<bool> notice_returned_ = false;
  mutable std::mutex mutex_;
  std::vector<EventIndexerPullRequest> requests_;
};

static_assert(is_EventIndexer1<CaughtUpPullRpc *, x23::MaybeAsync>);

TEST(IndexerPullController, RetriesAppliesBlockAndConfirmsCurrentProgress)
{
  TmpDir indexer_dir;
  auto db = IndexDB::make_new(std::filesystem::path(indexer_dir.directory()) /
                              "indexer-db");
  ASSERT_TRUE(db.has_value()) << db.error();
  RecentTxFailureCache failures;
  RpcServer<CaughtUpPullRpc> event_server;
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  IndexerPullControllerOptions options;
  options.retry_delay = std::chrono::milliseconds(1);
  options.caught_up_poll_delay = std::chrono::seconds(1);

  IndexerPullController controller(
      std::string(x23::HostPort{"127.0.0.1", event_port}.show()), *db, failures,
      options);
  controller.wait_until_caught_up();

  const std::vector<EventIndexerPullRequest> requests =
      event_server.impl()->requests();
  ASSERT_GE(requests.size(), 3UL);
  EXPECT_EQ(std::string_view(requests[0].protocol), version::kHogdProtocol());
  EXPECT_EQ(requests[0].progress.state(),
            IndexerProgressState::IndexerProgressUninitialized);
  EXPECT_EQ(requests[1].progress.state(),
            IndexerProgressState::IndexerProgressUninitialized);
  ASSERT_EQ(requests[2].progress.state(),
            IndexerProgressState::IndexerProgressInitialized);
  EXPECT_EQ(requests[2].progress.initialized().origin,
            event_server.impl()->origin());
  EXPECT_EQ(requests[2].progress.initialized().contiguous_height, 1);
  EXPECT_EQ(db->durable_progress(), requests[2].progress);

  auto diagnostic = failures.lookup(event_server.impl()->failure_hash());
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic->conflict.code,
            DbConflictCode::kDbConflictValueMismatch);
}

} // namespace
} // namespace hogd
