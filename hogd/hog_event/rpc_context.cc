#include "hogd/hog_event/rpc_context.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_conflict.hh"
#include "event_outbox_internal.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/time.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/event_block_context.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/hog_event/status.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "proposal.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

template<typename T>
bool
xdr_equal(const T &lhs, const T &rhs)
{
  return x23::serialize(lhs) == x23::serialize(rhs);
}

bool
candidate_matches(const IndexerOutboxCandidatePayload &candidate,
                  BlockID block_id, BlockHeight block_height,
                  const ProposalTxsHash &proposal_txs_hash)
{
  return candidate.block_id == block_id &&
         candidate.block_height == block_height &&
         candidate.proposal_txs_hash == proposal_txs_hash;
}

IndexerOutboxStore
open_indexer_outbox_store(std::string_view work_directory)
{
  auto store = IndexerOutboxStore::open_or_create_in(work_directory);
  if (!store) {
    throw std::runtime_error(
        std::format("failed to open indexer outbox: {}", store.error()));
  }
  return std::move(*store);
}

EventIndexerPullResult
pull_failure(EventIndexerPullOutcome outcome, std::string diagnostic)
{
  if (diagnostic.size() > 1024) {
    diagnostic.resize(1024);
  }
  EventIndexerPullResult result(outcome);
  result.diagnostic() =
      IndexerFailureMessage(diagnostic.data(), diagnostic.size());
  return result;
}

EventIndexerPullResult
pull_caught_up(const IndexerOutboxOrigin &origin)
{
  EventIndexerPullResult result(
      EventIndexerPullOutcome::EventIndexerPullCaughtUp);
  result.origin() = origin;
  return result;
}

EventIndexerPullResult
pull_block(IndexerOutboxOrigin origin, IndexerCommittedBlock block)
{
  EventIndexerPullResult result(EventIndexerPullOutcome::EventIndexerPullBlock);
  result.pulled_block() = EventIndexerPulledBlock{
      .origin = std::move(origin),
      .block = std::move(block),
  };
  return result;
}

IndexerOutboxOrigin
pull_origin(const IndexerOutboxEpochOriginInfo &origin)
{
  return {
      .protocol =
          ProtocolCommit(origin.protocol.data(), origin.protocol.size()),
      .epoch_sequence = origin.epoch_sequence,
      .first_height = origin.first_height,
      .first_prev_hash = origin.first_prev_hash,
  };
}

} // namespace

EventRPCContext::EventRPCContext(std::string_view work_directory)
  : work_directory_(work_directory),
    indexer_outbox_store_(open_indexer_outbox_store(work_directory))
{
  tx_failure_mailbox_.reserve(kMaxTxFailureNotices);
}

x23::Async<EventStatus<void>>
EventRPCContext::open_block(BlockID block_id)
{
  auto candidate_exists = indexer_outbox_store_.has_candidate(block_id);
  if (!candidate_exists) {
    error("failed to inspect candidate indexer outbox record for block {}: {}",
          block_id, candidate_exists.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  if (*candidate_exists) {
    co_return EventError{EventErrorCode::kExists};
  }

  info("EventRPCContext::open_block: block_id={} acquiring lock", block_id);
  x23::CoroGuard g;
  co_await g(lock_);
  info("EventRPCContext::open_block: block_id={} lock acquired", block_id);
  if (block_operations_.contains(block_id)) {
    co_return EventError{EventErrorCode::kExists};
  }

  // try_emplace, not emplace: emplace may construct the context node before
  // discovering the key exists (libstdc++ does), and destroying that
  // still-kActive context trips the assert in ~EventBlockContext. try_emplace
  // guarantees no construction -- and no move from cached_blocks_.back() --
  // unless the insertion happens.
  bool inserted = false;
  if (!cached_blocks_.empty()) {
    std::tie(std::ignore, inserted) =
        event_block_contexts_.try_emplace(block_id, work_directory_, block_id,
                                          std::move(cached_blocks_.back()));
    if (inserted) {
      cached_blocks_.pop_back();
    }
  }
  else {
    std::tie(std::ignore, inserted) =
        event_block_contexts_.try_emplace(block_id, work_directory_, block_id);
  }

  if (!inserted) {
    co_return EventError{EventErrorCode::kExists};
  }
  co_return EventStatus<void>();
}

x23::Async<EventStatus<SHA256Hash>>
EventRPCContext::close_block(BlockID block_id, BlockHeight block_height,
                             ProposalTxsHash proposal_txs_hash)
{
  if (!co_await reserve_block_operation(block_id, BlockOperation::kClosing)) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  if (close_reserved_test_hook_) {
    close_reserved_test_hook_(block_id);
  }

  auto candidate_exists = indexer_outbox_store_.has_candidate(block_id);
  if (!candidate_exists) {
    error("failed to inspect candidate indexer outbox record for block {}: {}",
          block_id, candidate_exists.error());
    co_await finish_block_operation(block_id, nullptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  if (*candidate_exists) {
    auto candidate = co_await indexer_outbox_store_.load_candidate(block_id);
    if (!candidate) {
      error("failed to load candidate indexer outbox record for block {}: {}",
            block_id, candidate.error());
      co_await finish_block_operation(block_id, nullptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!candidate_matches(*candidate, block_id, block_height,
                           proposal_txs_hash)) {
      co_await finish_block_operation(block_id, nullptr, false);
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    SHA256Hash root = candidate->event_root;
    co_await finish_block_operation(block_id, nullptr, false);
    co_return root;
  }

  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_wait_executors_closed = 0.0;
  double stage_wait_lock = 0.0;
  double stage_prepare_context_and_async = 0.0;

  PerThreadARC<EventBlockContext> *context = nullptr;
  {
    x23::CoroGuard g;
    co_await g(lock_);
    auto it = event_block_contexts_.find(block_id);
    if (it == event_block_contexts_.end() ||
        !it->second.try_acquire_reference()) {
      block_operations_.erase(block_id);
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    context = &it->second;
  }

  bool executors_closed = co_await context->get().await_all_executors_closed();
  record_stage(stage_wait_executors_closed);
  if (!executors_closed) {
    co_await finish_block_operation(block_id, context, false);
    co_return EventError{EventErrorCode::kBadBlockID};
  }

  {
    x23::CoroGuard g;
    co_await g(lock_);
    record_stage(stage_wait_lock);

    context->get().set_block_height(block_height);
    context->get().set_closing();
  }

  SHA256Hash out_hash;
  try {
    co_await context->get().finish_file_write();
    out_hash = context->get().get_root_hash();
    IndexerOutboxCandidatePayload candidate =
        co_await context->get().snapshot_indexer_outbox_candidate(
            block_height, out_hash, std::move(proposal_txs_hash));
    if (auto published =
            co_await indexer_outbox_store_.publish_candidate(candidate);
        !published) {
      panic("failed to publish candidate indexer outbox record: {}",
            published.error());
    }
    co_await context->get().finish_file(out_hash);
  } catch (const std::exception &e) {
    panic("failed to durably close event block {}: {}", block_id, e.what());
  } catch (...) {
    panic("failed to durably close event block {} with unknown exception",
          block_id);
  }
  co_await finish_block_operation(block_id, context, false);
  record_stage(stage_prepare_context_and_async);

  info("close_block() stages: wait_executors_closed={:.6f}, "
       "wait_lock={:.6f}, prepare_context_and_async={:.6f}, total={:.6f}, "
       "outcome={}, block_id={}",
       stage_wait_executors_closed, stage_wait_lock,
       stage_prepare_context_and_async, timer.elapsed(), "ok", block_id);
  co_return out_hash;
}

x23::Async<EventStatus<BlockHeight>>
EventRPCContext::commit_block(BlockID block_id, ConsensusValue consensus_value)
{
  if (!co_await reserve_block_operation(block_id,
                                        BlockOperation::kCommitting)) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }

  info("EventRPCContext::commit_block: block_id={} acquiring lock", block_id);
  PerThreadARC<EventBlockContext> *context_ptr = nullptr;
  {
    x23::CoroGuard g;
    co_await g(lock_);
    info("EventRPCContext::commit_block: block_id={} lock acquired", block_id);
    auto it = event_block_contexts_.find(block_id);
    if (it != event_block_contexts_.end()) {
      context_ptr = &it->second;
      if (!context_ptr->get().closing() ||
          !context_ptr->try_acquire_reference()) {
        block_operations_.erase(block_id);
        co_return EventError{EventErrorCode::kBadBlockID};
      }
    }
  }

  const BlockHeight block_height = consensus_value.proposal_header.block_height;
  x23::CoroGuard outbox_guard;
  co_await outbox_guard(outbox_commit_lock_);
  auto committed_exists = indexer_outbox_store_.has_committed(block_height);
  if (!committed_exists) {
    error("failed to inspect committed indexer outbox record at height {}: {}",
          block_height, committed_exists.error());
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  IndexerOutboxCommittedPayload committed;
  if (*committed_exists) {
    auto loaded = co_await indexer_outbox_store_.load_committed(block_height);
    if (!loaded) {
      error("failed to load committed indexer outbox record at height {}: {}",
            block_height, loaded.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!xdr_equal(loaded->consensus_value, consensus_value)) {
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    committed = std::move(*loaded);
  }
  else {
    auto promotion = co_await find_canonical_candidate(consensus_value);
    if (!promotion) {
      error("failed to identify canonical candidate for height {}: {}",
            block_height, promotion.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!*promotion) {
      error("no canonical candidate exists for committed height {}",
            block_height);
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    committed = std::move((*promotion)->committed);
    if (auto published =
            co_await indexer_outbox_store_.publish_committed(committed);
        !published) {
      error("failed to publish committed indexer outbox record for block {}: "
            "{}",
            block_id, published.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
  }
  if (auto removed = co_await remove_resolved_candidates(block_height);
      !removed) {
    error("failed to remove resolved candidate indexer outbox records through "
          "height {}: {}",
          block_height, removed.error());
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  // Startup recovery can durably receive a finalized commit before HogDB's
  // tip has been reconciled. Keep it invisible until reconciliation installs
  // a tail; once reconciled, commit itself is the publication barrier.
  if (finalized_indexer_tail_ && block_height < *finalized_indexer_tail_) {
    error("stale indexer publication at height {} below tail {}", block_height,
          *finalized_indexer_tail_);
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  if (finalized_indexer_tail_ && block_height != *finalized_indexer_tail_ &&
      (*finalized_indexer_tail_ == INT64_MAX ||
       block_height != *finalized_indexer_tail_ + 1)) {
    error("indexer publication skipped from height {} to {}",
          *finalized_indexer_tail_, block_height);
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  if (finalized_indexer_tail_) {
    finalized_indexer_tail_ = block_height;
  }

  outbox_guard.reset();

  info("EventRPCContext::commit_block: block_id={} done, gc", block_id);
  co_await finish_block_operation(block_id, context_ptr,
                                  context_ptr != nullptr);
  co_return block_height;
}

x23::Async<EventStatus<void>>
EventRPCContext::reconcile_outbox_tip(BlockID block_id,
                                      ConsensusValue consensus_value)
{
  outbox_reconciled_.store(false, std::memory_order_release);
  const BlockHeight tip_height = consensus_value.proposal_header.block_height;
  const ConsensusValueHash tip_hash =
      tip_height == -1 ? ConsensusValueHash{} : sha256(consensus_value);
  const std::string current_protocol(version::kHogdProtocol());
  const BlockHeight max_allowed_height =
      tip_height == INT64_MAX ? INT64_MAX : tip_height + 1;

  // Validation phase. No durable state is changed until every retained origin
  // and committed record, the protocol sequence, candidate promotion boundary,
  // and scheduler-tip relationship have passed.
  auto history =
      co_await indexer_outbox_store_
          .validate_and_cache_committed_history_snapshot(max_allowed_height);
  if (!history) {
    error("failed to validate committed indexer outbox history: {}",
          history.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  auto *records = &history->records;
  auto *origins = &history->origins;
  auto *current_origin = &history->current_origin;
  auto candidates = indexer_outbox_store_.enumerate_candidate_records_raw();
  if (!candidates) {
    error("failed to enumerate candidate indexer outbox records: {}",
          candidates.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  const auto is_canonical_candidate = [&](const auto &candidate) {
    return candidate.block_height == tip_height &&
           candidate.event_root ==
               consensus_value.proposal_header.events_hash &&
           candidate.proposal_txs_hash == consensus_value.proposal_txs_hash;
  };
  bool committed_tip_present =
      std::ranges::any_of(*records, [&](const auto &record) {
        return record.protocol == current_protocol &&
               record.block_height == tip_height &&
               record.consensus_hash == tip_hash;
      });
  const bool archival_candidate_matches =
      std::ranges::any_of(*candidates, [&](const auto &candidate) {
        return candidate.protocol != current_protocol &&
               is_canonical_candidate(candidate);
      });
  auto promotion = co_await find_canonical_candidate(consensus_value);
  if (!promotion) {
    error("failed to validate canonical retained candidate: {}",
          promotion.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  const bool promotion_needed =
      promotion->has_value() && !committed_tip_present;
  if (archival_candidate_matches && !committed_tip_present &&
      !promotion_needed) {
    error("canonical candidate uses an archival outbox protocol; migration is "
          "required");
    co_return EventError{EventErrorCode::kInternal};
  }

  std::optional<IndexerOutboxCommittedRecordInfo> previous_current;
  if (history->latest && history->latest->protocol == current_protocol) {
    previous_current = history->latest;
  }
  const bool saw_current_epoch = previous_current.has_value();
  const BlockHeight last_current_height =
      previous_current ? previous_current->block_height : -1;
  std::optional<IndexerOutboxCommittedRecordInfo> previous_record =
      history->latest;
  std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary =
      history->canonical_boundary;

  std::optional<std::pair<BlockHeight, ConsensusValueHash>> planned_origin;
  if (promotion_needed) {
    CHECK(promotion->has_value());
    if (tip_height < 0 ||
        (canonical_boundary &&
         (tip_height != canonical_boundary->height ||
          consensus_value.prev_hash != canonical_boundary->consensus_hash)) ||
        (previous_record &&
         (previous_record->block_height == INT64_MAX ||
          tip_height != previous_record->block_height + 1 ||
          consensus_value.prev_hash != previous_record->consensus_hash))) {
      error("canonical retained candidate does not append to validated outbox "
            "history");
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!previous_record && saw_current_epoch) {
      error("current outbox epoch state is internally inconsistent");
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!saw_current_epoch) {
      if (*current_origin) {
        if ((*current_origin)->origin.first_height != tip_height ||
            (*current_origin)->origin.first_prev_hash !=
                consensus_value.prev_hash) {
          error("current outbox origin does not match retained candidate");
          co_return EventError{EventErrorCode::kInternal};
        }
      }
      else {
        error("retained candidate promotion requires an established current "
              "outbox origin");
        co_return EventError{EventErrorCode::kInternal};
      }
    }
  }
  else if (previous_current) {
    if (last_current_height < tip_height) {
      error("current outbox epoch ends at {}, behind canonical tip {}",
            last_current_height, tip_height);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (last_current_height == tip_height) {
      if (previous_current->consensus_hash != tip_hash) {
        error(
            "committed outbox tip does not match the canonical scheduler tip");
        co_return EventError{EventErrorCode::kInternal};
      }
    }
    else if (tip_height != INT64_MAX && last_current_height == tip_height + 1) {
      if (previous_current->prev_hash != tip_hash) {
        error("single outbox-ahead record does not link to scheduler tip");
        co_return EventError{EventErrorCode::kInternal};
      }
      info("finalized event outbox is one block ahead of HogDB at height {}",
           last_current_height);
    }
    else {
      error("invalid committed outbox-ahead state");
      co_return EventError{EventErrorCode::kInternal};
    }
  }
  else if (*current_origin) {
    if (tip_height == INT64_MAX ||
        (*current_origin)->origin.first_height != tip_height + 1 ||
        (*current_origin)->origin.first_prev_hash != tip_hash ||
        (canonical_boundary && ((*current_origin)->origin.first_height !=
                                    canonical_boundary->height ||
                                (*current_origin)->origin.first_prev_hash !=
                                    canonical_boundary->consensus_hash)) ||
        (previous_record && (previous_record->block_height == INT64_MAX ||
                             (*current_origin)->origin.first_height !=
                                 previous_record->block_height + 1 ||
                             (*current_origin)->origin.first_prev_hash !=
                                 previous_record->consensus_hash))) {
      error("current outbox epoch origin without records is not the canonical "
            "next publication");
      co_return EventError{EventErrorCode::kInternal};
    }
  }
  else if (previous_record) {
    if (previous_record->block_height != tip_height ||
        previous_record->consensus_hash != tip_hash ||
        tip_height == INT64_MAX) {
      error("validated archival outbox does not end at the canonical scheduler "
            "tip");
      co_return EventError{EventErrorCode::kInternal};
    }
    planned_origin = {tip_height + 1, tip_hash};
  }
  else if (canonical_boundary) {
    if (tip_height == INT64_MAX ||
        canonical_boundary->height != tip_height + 1 ||
        canonical_boundary->consensus_hash != tip_hash) {
      error("all-empty archival outbox boundary does not match the canonical "
            "scheduler tip");
      co_return EventError{EventErrorCode::kInternal};
    }
    planned_origin = {canonical_boundary->height,
                      canonical_boundary->consensus_hash};
  }
  else if (tip_height == 0) {
    if (indexer_outbox_store_.recognized_temporary_residue_on_open()) {
      error("refusing to initialize genesis outbox origin after cleaning "
            "recognized temporary residue");
      co_return EventError{EventErrorCode::kInternal};
    }
    planned_origin = {1, tip_hash};
  }
  else if (tip_height != -1) {
    error("new outbox epoch without history is not linked to the canonical "
          "scheduler tip");
    co_return EventError{EventErrorCode::kInternal};
  }

  // Mutation phase. Every rejection above preserves candidates and committed
  // records byte-for-byte.
  if (planned_origin) {
    auto initialized =
        co_await indexer_outbox_store_.initialize_current_epoch_origin(
            planned_origin->first, planned_origin->second);
    if (!initialized) {
      error("failed to initialize current indexer outbox epoch origin: {}",
            initialized.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    auto loaded_current_origin =
        indexer_outbox_store_.load_current_epoch_origin();
    if (!loaded_current_origin || !*loaded_current_origin) {
      error("current indexer outbox epoch origin was not durably initialized");
      co_return EventError{EventErrorCode::kInternal};
    }
    *current_origin = std::move(**loaded_current_origin);
    const IndexerOutboxEpochOriginInfo initialized_origin{
        .protocol = current_protocol,
        .epoch_sequence = (**current_origin).origin.epoch_sequence,
        .first_height = (**current_origin).origin.first_height,
        .first_prev_hash = (**current_origin).origin.first_prev_hash,
    };
    origins->push_back(initialized_origin);
  }

  if (promotion_needed) {
    CHECK(promotion->has_value());
    auto promoted = co_await commit_block(block_id, consensus_value);
    if (!promoted) {
      co_return promoted.error();
    }
    auto promoted_record =
        co_await indexer_outbox_store_.lookup_validated_committed_record(
            tip_height);
    if (!promoted_record || !*promoted_record ||
        (**promoted_record).protocol != current_protocol ||
        (**promoted_record).consensus_hash != tip_hash ||
        (**promoted_record).prev_hash != consensus_value.prev_hash) {
      error("promoted retained candidate is not the committed outbox tip");
      co_return EventError{EventErrorCode::kInternal};
    }
    previous_record = std::move(**promoted_record);
    if (previous_record->block_height == INT64_MAX) {
      canonical_boundary.reset();
    }
    else {
      canonical_boundary = IndexerOutboxCanonicalBoundary{
          .height = previous_record->block_height + 1,
          .consensus_hash = previous_record->consensus_hash,
      };
    }
  }

  x23::CoroGuard outbox_guard;
  co_await outbox_guard(outbox_commit_lock_);
  auto stable =
      co_await indexer_outbox_store_.cache_validated_committed_append_tip(
          previous_record, *current_origin, canonical_boundary);
  if (!stable) {
    error("committed indexer outbox advanced during reconciliation: {}",
          stable.error());
    co_return EventError{EventErrorCode::kInternal};
  }

  if (previous_record) {
    auto removed =
        co_await remove_resolved_candidates(previous_record->block_height);
    if (!removed) {
      error("failed to clean reconciled candidates through height {}: {}",
            previous_record->block_height, removed.error());
      co_return EventError{EventErrorCode::kInternal};
    }
  }

  // eventsched_commit_block is called only after finalization, so a retained
  // record from the event/HogDB parallel-commit crash window remains safe to
  // publish even when it is one block ahead of the recovered HogDB tip.
  finalized_indexer_tail_ =
      previous_record ? previous_record->block_height : tip_height;
  current_indexer_origin_.reset();
  if (*current_origin) {
    current_indexer_origin_ = (**current_origin).origin;
  }
  indexer_origins_ = std::move(*origins);
  outbox_reconciled_.store(true, std::memory_order_release);
  co_return EventStatus<void>();
}

x23::Async<EventIndexerPullResult>
EventRPCContext::pull_indexer_next(const EventIndexerPullRequest &request)
{
  using enum EventIndexerPullOutcome;
  if (std::string_view(request.protocol) != version::kHogdProtocol()) {
    co_return pull_failure(EventIndexerPullMismatchedProtocol,
                           "event/indexer protocol mismatch");
  }
  if (!outbox_reconciled_.load(std::memory_order_acquire)) {
    co_return pull_failure(EventIndexerPullNotReady,
                           "event outbox is not reconciled");
  }

  IndexerOutboxOrigin current_origin;
  IndexerOutboxOrigin response_origin;
  std::optional<IndexerOutboxEpochOriginInfo> retained_origin;
  std::optional<IndexerOutboxEpochOriginInfo> next_retained_origin;
  BlockHeight finalized_tail = -1;
  {
    x23::CoroGuard guard;
    co_await guard(outbox_commit_lock_);
    if (!outbox_reconciled_.load(std::memory_order_acquire) ||
        !current_indexer_origin_ || !finalized_indexer_tail_) {
      co_return pull_failure(EventIndexerPullNotReady,
                             "event outbox origin is unavailable");
    }
    current_origin = *current_indexer_origin_;
    finalized_tail = *finalized_indexer_tail_;
    if (request.progress.state() ==
        IndexerProgressState::IndexerProgressInitialized) {
      const auto it =
          std::ranges::find_if(indexer_origins_, [&](const auto &o) {
            return o.protocol ==
                   std::string_view(
                       request.progress.initialized().origin.protocol);
          });
      if (it != indexer_origins_.end()) {
        retained_origin = *it;
        if (std::next(it) != indexer_origins_.end()) {
          next_retained_origin = *std::next(it);
        }
      }
    }
    else {
      CHECK(!indexer_origins_.empty());
      response_origin = pull_origin(indexer_origins_.front());
    }
  }

  BlockHeight next_height = response_origin.first_height;
  if (request.progress.state() ==
      IndexerProgressState::IndexerProgressInitialized) {
    const IndexerInitializedProgress &progress = request.progress.initialized();
    if (!retained_origin) {
      co_return pull_failure(EventIndexerPullProgressConflict,
                             "indexer progress uses an unknown outbox origin");
    }
    if (retained_origin->protocol !=
        std::string_view(progress.origin.protocol)) {
      co_return pull_failure(EventIndexerPullProgressConflict,
                             "indexer progress protocol conflicts with origin");
    }
    if (retained_origin->epoch_sequence != progress.origin.epoch_sequence) {
      co_return pull_failure(
          EventIndexerPullProgressConflict,
          "indexer progress epoch sequence conflicts with origin");
    }
    if (retained_origin->first_height != progress.origin.first_height) {
      co_return pull_failure(EventIndexerPullProgressConflict,
                             "indexer progress height conflicts with origin");
    }
    if (retained_origin->first_prev_hash != progress.origin.first_prev_hash) {
      co_return pull_failure(
          EventIndexerPullProgressConflict,
          "indexer progress parent hash conflicts with origin");
    }
    if (progress.contiguous_height < progress.origin.first_height - 1 ||
        progress.contiguous_height > finalized_tail) {
      co_return pull_failure(EventIndexerPullProgressConflict,
                             "indexer progress height is outside the outbox");
    }

    const BlockHeight origin_boundary = progress.origin.first_height - 1;
    if (progress.contiguous_height < 0) {
      if (progress.contiguous_height != origin_boundary ||
          progress.last_consensus_hash != progress.origin.first_prev_hash ||
          progress.last_payload_digest != SHA256Hash{}) {
        co_return pull_failure(
            EventIndexerPullProgressConflict,
            "indexer progress conflicts with epoch boundary");
      }
    }
    else {
      auto progress_record =
          co_await indexer_outbox_store_.lookup_validated_committed_record(
              progress.contiguous_height);
      if (!progress_record) {
        error("failed to inspect pulled indexer progress at height {}: {}",
              progress.contiguous_height, progress_record.error());
        co_return pull_failure(EventIndexerPullInternalFailure,
                               "durable outbox progress record is unavailable");
      }
      const bool empty_epoch_boundary =
          progress.contiguous_height == origin_boundary &&
          progress.last_consensus_hash == progress.origin.first_prev_hash &&
          progress.last_payload_digest == SHA256Hash{};
      bool validate_retained_record = true;
      if (empty_epoch_boundary && !*progress_record) {
        validate_retained_record = false;
      }
      if (validate_retained_record) {
        if (!*progress_record) {
          error("validated indexer progress record is unavailable at height {}",
                progress.contiguous_height);
          co_return pull_failure(
              EventIndexerPullInternalFailure,
              "durable outbox progress record is unavailable");
        }
        if ((**progress_record).block_height != progress.contiguous_height ||
            (**progress_record).consensus_hash !=
                progress.last_consensus_hash ||
            (**progress_record).payload_digest !=
                progress.last_payload_digest ||
            (progress.contiguous_height == origin_boundary &&
             progress.last_consensus_hash != progress.origin.first_prev_hash) ||
            (progress.contiguous_height > origin_boundary &&
             (**progress_record).protocol !=
                 std::string_view(progress.origin.protocol))) {
          co_return pull_failure(
              EventIndexerPullProgressConflict,
              "indexer progress identity conflicts with retained outbox");
        }
      }
    }

    response_origin = progress.origin;
    if (progress.origin == current_origin) {
      if (progress.contiguous_height == INT64_MAX) {
        next_height = INT64_MAX;
      }
      else {
        next_height = progress.contiguous_height + 1;
      }
    }
    else {
      if (!next_retained_origin || progress.contiguous_height == INT64_MAX) {
        co_return pull_failure(EventIndexerPullProgressConflict,
                               "indexer progress has no valid successor epoch");
      }
      const IndexerOutboxOrigin next_origin =
          pull_origin(*next_retained_origin);
      next_height = progress.contiguous_height + 1;
      if (next_height > next_origin.first_height) {
        co_return pull_failure(
            EventIndexerPullProgressConflict,
            "indexer progress crosses its successor epoch boundary");
      }
      if (next_height == next_origin.first_height) {
        if (progress.last_consensus_hash != next_origin.first_prev_hash) {
          co_return pull_failure(
              EventIndexerPullProgressConflict,
              "indexer progress does not reach its successor epoch boundary");
        }
        response_origin = next_origin;
      }
    }
  }

  if (next_height > finalized_tail ||
      (next_height == INT64_MAX &&
       request.progress.state() ==
           IndexerProgressState::IndexerProgressInitialized &&
       request.progress.initialized().contiguous_height == INT64_MAX)) {
    x23::CoroGuard guard;
    co_await guard(outbox_commit_lock_);
    if (!outbox_reconciled_.load(std::memory_order_acquire) ||
        !current_indexer_origin_ || !finalized_indexer_tail_) {
      co_return pull_failure(EventIndexerPullNotReady,
                             "event outbox origin is unavailable");
    }
    if (*current_indexer_origin_ != current_origin) {
      co_return pull_failure(EventIndexerPullNotReady,
                             "event outbox origin changed");
    }
    finalized_tail = *finalized_indexer_tail_;
    if (next_height > finalized_tail ||
        (next_height == INT64_MAX &&
         request.progress.state() ==
             IndexerProgressState::IndexerProgressInitialized &&
         request.progress.initialized().contiguous_height == INT64_MAX)) {
      co_return pull_caught_up(response_origin);
    }
  }

  auto record =
      co_await indexer_outbox_store_.lookup_validated_committed_record(
          next_height);
  if (!record) {
    error("failed to inspect pullable committed outbox record {}: {}",
          next_height, record.error());
    co_return pull_failure(EventIndexerPullInternalFailure,
                           "pullable durable outbox record is unavailable");
  }
  if (!*record) {
    error("pullable committed outbox record {} is missing", next_height);
    co_return pull_failure(EventIndexerPullInternalFailure,
                           "pullable durable outbox record is unavailable");
  }
  if ((**record).protocol != std::string_view(response_origin.protocol)) {
    if (response_origin != current_origin) {
      x23::CoroGuard guard;
      co_await guard(outbox_commit_lock_);
      if (!outbox_reconciled_.load(std::memory_order_acquire) ||
          !current_indexer_origin_ ||
          *current_indexer_origin_ != current_origin) {
        co_return pull_failure(EventIndexerPullNotReady,
                               "event outbox state changed during pull");
      }
      co_return pull_caught_up(response_origin);
    }
    error("pullable committed outbox record {} has the wrong epoch",
          next_height);
    co_return pull_failure(EventIndexerPullInternalFailure,
                           "pullable durable outbox record has wrong epoch");
  }
  auto loaded = co_await indexer_outbox_store_.load_committed_record(
      next_height, std::string_view(response_origin.protocol));
  if (!loaded) {
    error("failed to load pullable committed outbox record {}: {}", next_height,
          loaded.error());
    co_return pull_failure(EventIndexerPullInternalFailure,
                           "pullable durable outbox payload is invalid");
  }
  if (loaded->info != **record) {
    error("pullable committed outbox record {} failed identity validation",
          next_height);
    co_return pull_failure(EventIndexerPullInternalFailure,
                           "pullable durable outbox identity is invalid");
  }
  // The loaded record has already validated payload/header hashes, so preserve
  // that durable identity instead of hashing the payload again.
  IndexerCommittedBlock block{
      .payload = std::move(loaded->payload),
      .identity =
          IndexerCommittedBlockIdentity{
              .consensus_hash = loaded->info.consensus_hash,
              .payload_digest = loaded->info.payload_digest,
          },
  };

  {
    x23::CoroGuard guard;
    co_await guard(outbox_commit_lock_);
    if (!outbox_reconciled_.load(std::memory_order_acquire) ||
        !current_indexer_origin_ ||
        *current_indexer_origin_ != current_origin ||
        !finalized_indexer_tail_ || *finalized_indexer_tail_ < next_height) {
      co_return pull_failure(EventIndexerPullNotReady,
                             "event outbox state changed during pull");
    }
  }
  co_return pull_block(std::move(response_origin), std::move(block));
}

void
EventRPCContext::enqueue_tx_failure(TransactionHash tx_hash,
                                    TxFailureDiagnostic diagnostic)
{
  std::lock_guard guard(tx_failure_mutex_);
  if (tx_failure_mailbox_.size() == kMaxTxFailureNotices) {
    warn_every_n_sec(5.0F, "tx-failure notice dropped: mailbox is full");
    return;
  }
  tx_failure_mailbox_.push_back(IndexerTxFailureNotice{
      .tx_hash = std::move(tx_hash),
      .diagnostic = std::move(diagnostic),
  });
}

IndexerTxFailureNoticeBatch
EventRPCContext::pull_tx_failures()
{
  IndexerTxFailureNoticeBatch notices;
  std::lock_guard guard(tx_failure_mutex_);
  notices.reserve(tx_failure_mailbox_.size());
  for (auto &notice : tx_failure_mailbox_) {
    notices.push_back(std::move(notice));
  }
  tx_failure_mailbox_.clear();
  return notices;
}

x23::Async<EventStatus<void>>
EventRPCContext::abandon_block(BlockID block_id)
{
  if (!co_await reserve_block_operation(block_id,
                                        BlockOperation::kAbandoning)) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  const auto clean_redundant_candidate =
      [&]() -> x23::Async<EventStatus<bool>> {
    auto candidate_exists = indexer_outbox_store_.has_candidate(block_id);
    if (!candidate_exists) {
      error("failed to inspect candidate indexer outbox record for block {}: "
            "{}",
            block_id, candidate_exists.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!*candidate_exists) {
      co_return false;
    }
    auto candidate = co_await indexer_outbox_store_.load_candidate(block_id);
    if (!candidate) {
      error("failed to load abandoned candidate {}: {}", block_id,
            candidate.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    auto committed_exists =
        indexer_outbox_store_.has_committed(candidate->block_height);
    if (!committed_exists) {
      error("failed to inspect committed outbox record at height {}: {}",
            candidate->block_height, committed_exists.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!*committed_exists) {
      co_return true;
    }
    auto committed =
        co_await indexer_outbox_store_.load_committed(candidate->block_height);
    if (!committed) {
      error("failed to validate committed outbox record at height {}: {}",
            candidate->block_height, committed.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    if (auto removed = indexer_outbox_store_.remove_candidate(block_id);
        !removed) {
      error("failed to remove redundant abandoned candidate {}: {}", block_id,
            removed.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    co_return true;
  };

  info("EventRPCContext::abandon_block: block_id={} acquiring lock", block_id);

  PerThreadARC<EventBlockContext> *context_ptr = nullptr;
  bool closing = false;
  {
    x23::CoroGuard g;
    co_await g(lock_);
    info("EventRPCContext::abandon_block: block_id={} lock acquired", block_id);

    auto it = event_block_contexts_.find(block_id);
    if (it != event_block_contexts_.end()) {
      context_ptr = &it->second;
      if (!context_ptr->try_acquire_reference()) {
        block_operations_.erase(block_id);
        co_return EventError{EventErrorCode::kBadBlockID};
      }
      closing = context_ptr->get().closing();
      CHECK(!context_ptr->initiate_shutdown());
    }
  }

  if (context_ptr == nullptr) {
    auto candidate = co_await clean_redundant_candidate();
    if (!candidate) {
      co_await finish_block_operation(block_id, nullptr, false);
      co_return candidate.error();
    }
    if (!*candidate) {
      co_await finish_block_operation(block_id, nullptr, false);
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    co_await finish_block_operation(block_id, nullptr, false);
    co_return EventStatus<void>();
  }

  if (closing) {
    co_await context_ptr->get().wait_file_done();
  }
  else {
    info("EventRPCContext::abandon_block: block_id={} awaiting abandon",
         block_id);
    co_await context_ptr->get().await_abandon();
    context_ptr->get().delete_events_file();
  }

  info("EventRPCContext::abandon_block: block_id={} done (post-close), gc",
       block_id);
  co_await finish_block_operation(block_id, context_ptr, true);
  auto candidate = co_await clean_redundant_candidate();
  if (!candidate) {
    co_return candidate.error();
  }
  co_return EventStatus<void>();
}

x23::Async<bool>
EventRPCContext::reserve_block_operation(BlockID block_id,
                                         BlockOperation operation)
{
  x23::CoroGuard g;
  co_await g(lock_);
  co_return block_operations_.try_emplace(block_id, operation).second;
}

x23::Async<IndexerOutboxStatus<void>>
EventRPCContext::remove_resolved_candidates(BlockHeight committed_height)
{
  auto candidates = indexer_outbox_store_.enumerate_candidate_records_raw();
  if (!candidates) {
    co_return std::unexpected(candidates.error());
  }

  for (const auto &candidate : *candidates) {
    if (candidate.block_height > committed_height) {
      continue;
    }
    if (candidate.protocol == std::string_view(version::kHogdProtocol())) {
      auto payload =
          co_await indexer_outbox_store_.load_candidate(candidate.block_id);
      if (!payload) {
        co_return std::unexpected(
            std::format("failed to validate current candidate {}: {}",
                        candidate.block_id, payload.error()));
      }
    }
    if (auto removed =
            indexer_outbox_store_.remove_candidate(candidate.block_id);
        !removed) {
      co_return std::unexpected(std::format("failed to remove candidate {}: {}",
                                            candidate.block_id,
                                            removed.error()));
    }
  }
  co_return IndexerOutboxStatus<void>{};
}

x23::Async<
    IndexerOutboxStatus<std::optional<EventRPCContext::CandidatePromotion>>>
EventRPCContext::find_canonical_candidate(
    const ConsensusValue &consensus_value) const
{
  auto records = indexer_outbox_store_.enumerate_candidate_records_raw();
  if (!records) {
    co_return std::unexpected(records.error());
  }
  const BlockHeight height = consensus_value.proposal_header.block_height;
  std::optional<CandidatePromotion> selected;
  for (const auto &record : *records) {
    if (record.protocol != std::string_view(version::kHogdProtocol()) ||
        record.block_height != height ||
        record.event_root != consensus_value.proposal_header.events_hash ||
        record.proposal_txs_hash != consensus_value.proposal_txs_hash) {
      continue;
    }
    // Candidate enumeration is deterministic. One matching candidate is
    // sufficient; full committed-payload validation below binds its events and
    // transactions to the finalized consensus value.
    auto candidate =
        co_await indexer_outbox_store_.load_candidate(record.block_id);
    if (!candidate) {
      co_return std::unexpected(
          std::format("failed to validate candidate {}: {}", record.block_id,
                      candidate.error()));
    }
    IndexerOutboxCommittedPayload committed{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = consensus_value,
        .event_logs = std::move(candidate->event_logs),
        .tx_bodies = std::move(candidate->tx_bodies),
    };
    selected = CandidatePromotion{.committed = std::move(committed)};
    break;
  }
  co_return selected;
}

x23::Async<void>
EventRPCContext::finish_block_operation(
    BlockID block_id, PerThreadARC<EventBlockContext> *context, bool shutdown)
{
  x23::CoroGuard g;
  co_await g(lock_);
  if (context != nullptr) {
    auto it = event_block_contexts_.find(block_id);
    CHECK(it != event_block_contexts_.end());
    CHECK(&it->second == context);
    bool released = context->release_reference();
    if (released || (shutdown && context->initiate_shutdown())) {
      gc_context_internal(it);
    }
  }
  block_operations_.erase(block_id);
}

x23::Async<void>
EventRPCContext::gc_context(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(lock_);

  auto it = event_block_contexts_.find(block_id);
  assert(it != event_block_contexts_.end());
  gc_context_internal(it);
}

void
EventRPCContext::gc_context_internal(
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator it)
{
  // the block must already have been shutdown by this point,
  // and should have no outstanding references
  auto thread_ul = cache_.lock_threads();
  cache_.for_each(thread_ul, [&](ContextCache &cache) {
    std::lock_guard g(cache.mtx_);
    cache.contexts_.erase(it->first);
  });

  if (cached_blocks_.size() < kMaxCachedBlocks) {
    cached_blocks_.emplace_back(it->second.get().extract_data());
  }

  event_block_contexts_.erase(it);
}

x23::Async<PerThreadARCRef<EventBlockContext>>
EventRPCContext::get_context(BlockID block_id)
{
  {
    PerThreadARC<EventBlockContext> *ctx = nullptr;
    bool found = false;
    {
      std::scoped_lock g(cache_->mtx_);
      if (auto it = cache_->contexts_.find(block_id);
          it != cache_->contexts_.end()) {
        found = true;
        if (it->second->try_acquire_reference()) {
          ctx = it->second;
        }
        // else ctx = nullptr; we found the context in the cache,
        // but it was already closed/closing, so we do not return a reference to
        // it.
      }
    }
    // Return after releasing cache_->mtx_.
    if (found) {
      co_return PerThreadARCRef<EventBlockContext>(ctx);
    }
  }
  auto *ctx = co_await get_context_slow(block_id);
  co_return PerThreadARCRef<EventBlockContext>(ctx);
}

x23::Async<PerThreadARC<EventBlockContext> *>
EventRPCContext::get_context_slow(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(lock_);

  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    // Avoid building an unbounded unlock->resume recursion chain under heavy
    // lock contention when many coroutines miss in the per-thread cache.
    // Prefer resuming the awaiting caller inline so this hot path avoids an
    // extra thread-pool handoff on return.
    co_await g.reset_and_return_prefer_caller(nullptr);
    panic("unreachable");
  }

  if (!it->second.try_acquire_reference()) {
    co_await g.reset_and_return_prefer_caller(nullptr);
    panic("unreachable");
  }

  // put the context in the threadlocal cache so we do not pay this cost next
  // time.
  {
    std::scoped_lock g(cache_->mtx_);
    cache_->contexts_.emplace(block_id, &it->second);
  }
  co_await g.reset_and_return_prefer_caller(&it->second);
  panic("unreachable");
}

} // namespace hogd
