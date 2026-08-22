#include "hogd/hog_event/rpc_context.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
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
#include "hogd/base/coropoll.h"
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
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

namespace {

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
EventRPCContext::close_block(BlockID block_id, BlockHeight block_height)
{
  if (!co_await reserve_block_operation(block_id, BlockOperation::kClosing)) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  if (close_reserved_test_hook_) {
    close_reserved_test_hook_(block_id);
  }

  // The scheduler closes a block exactly once. A second close is a scheduler
  // bug, not a retry to absorb: set_block_height() below fail-stops on it.

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
    // Overlap the events-file drain with root-hash computation. Neither
    // depends on the other: await_all_executors_closed() has already returned
    // and add_event() refuses once the last executor handle is gone, so no
    // further events can arrive and the trie is already final.
    // finish_file_write() is I/O bound (it flushes the residual per-thread
    // buffers and fsyncs the events file) while get_root_hash() is CPU-bound
    // parallel Merkle hashing.
    //
    // The bg() hop is what makes the overlap real. x23::Async is eager, so
    // without it finish_file_write() would run inline on this thread through
    // its fsync before get_root_hash() ever started -- which is what the
    // straight-line version did.
    auto file_write = [](EventBlockContext &ctx) -> x23::Async<void> {
      auto *pool = x23::ThreadPool::get_thread_pool();
      if (pool == nullptr) {
        pool = &default_threadpool();
      }
      co_await pool->bg();
      co_await ctx.finish_file_write();
    }(context->get());

    // Always drain file_write before leaving this scope: an in-flight
    // x23::Async must not be destroyed un-awaited, and letting that assert
    // fire during unwinding would mask the real hashing failure.
    std::exception_ptr hash_ex;
    try {
      out_hash = context->get().get_root_hash();
    } catch (...) {
      hash_ex = std::current_exception();
    }
    co_await file_write;
    if (hash_ex) {
      std::rethrow_exception(hash_ex);
    }

    // add_event() streamed every event/body pair as it ingested them, so by now
    // the block's payload is already on disk. All that is left is to make it
    // durable and give it its content-addressed name: finish_file_write() above
    // fsynced the bytes, and finish_file() renames to <event root>.events and
    // fsyncs the directory.
    //
    // That file is the block's durable record. Promotion at commit opens it by
    // the finalized ProposalHeader.events_hash, which is exactly this name, so
    // a crash between close and commit loses nothing.
    // Claim the name before publishing under it, not after.  Another block
    // with the same event root may already have published this file and be
    // tearing down; if it dropped the last claim in the window between our
    // link() and our claim, it would unlink the file we are about to depend
    // on at commit.  Claiming first means the count never reaches zero while
    // this block still needs the name.
    {
      x23::CoroGuard g;
      co_await g(lock_);
      retain_events_file(out_hash);
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
  if (*committed_exists) {
    // An idempotent retry: the record's header carries the consensus hash, so
    // the retry is matched without reading the payload back.
    auto info =
        indexer_outbox_store_.load_committed_record_info_raw(block_height);
    if (!info) {
      error("failed to load committed indexer outbox record at height {}: {}",
            block_height, info.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (info->consensus_hash != sha256(consensus_value)) {
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kBadBlockID};
    }
  }
  else if (context_ptr != nullptr) {
    // The live commit path publishes the record by reference. This block
    // computed the event root itself over the exact pairs it streamed, and
    // close made the file durable before returning, so there is nothing to
    // read back or recompute -- the root only has to match the finalized
    // header, and the store still probes that the referenced file exists and
    // fits the pull RPC's replay record.
    if (!context_ptr->get().has_root_hash() ||
        context_ptr->get().root_hash() !=
            consensus_value.proposal_header.events_hash) {
      error("commit_block: block {} root does not match finalized events_hash",
            block_id);
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    const IndexerOutboxCommittedStoredPayload stored{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = consensus_value,
    };
    if (auto published =
            co_await indexer_outbox_store_.publish_committed_reference(stored);
        !published) {
      error("failed to publish committed indexer outbox record for block {}: "
            "{}",
            block_id, published.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
  }
  else {
    // Restart promotion: no live context vouches for the file, so read it
    // back and validate the full payload. The file is content-addressed by
    // the event root and the finalized ConsensusValue carries exactly that
    // root, so there is nothing to search for.
    auto pairs =
        read_final_events_file(work_directory_,
                               consensus_value.proposal_header.events_hash);
    if (!pairs) {
      error("failed to read events file for committed height {}: {}",
            block_height, pairs.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    IndexerOutboxCommittedPayload committed{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = consensus_value,
        .event_logs = std::move(pairs->event_logs),
        .tx_bodies = std::move(pairs->tx_bodies),
    };
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

  // The committed record references the events file rather than embedding the
  // pairs, so the file must outlive every block context that shares its root.
  // Take a claim on the record's behalf; it is never released, matching the
  // record's own retention. finish_block_operation below then releases the
  // block's claim without the count reaching zero.
  {
    x23::CoroGuard g;
    co_await g(lock_);
    retain_events_file(consensus_value.proposal_header.events_hash);
  }

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
  // and committed record, the protocol sequence, the promotion boundary,
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
  // Whether this consensus value has been committed at all, under any protocol.
  //
  // Protocol is deliberately not part of this test. Promotion has to key off
  // "has this been committed", because the events file cannot answer it: unlike
  // the candidate record it replaces, the events file is retained after commit
  // rather than removed, so its presence says nothing about commit state.
  // Matching on the current protocol as well would re-promote every
  // already-committed height whose record is tagged with an archival protocol.
  const bool committed_under_any_protocol =
      std::ranges::any_of(*records, [&](const auto &record) {
        return record.block_height == tip_height &&
               record.consensus_hash == tip_hash;
      });

  // When the scheduler's tip is not in the committed outbox at all, the block's
  // events file is what recovers it: the file is content-addressed by the event
  // root, and the finalized ConsensusValue carries exactly that root. A missing
  // file simply means there is nothing to promote. A file that exists but does
  // not parse is fatal, and is rejected here in the validation phase so that no
  // durable state changes first; commit_block() re-reads it to promote.
  bool promotion_needed = false;
  if (!committed_under_any_protocol && tip_height >= 0) {
    const std::string events_file = EventBlockContext::final_events_filename(
        work_directory_, consensus_value.proposal_header.events_hash);
    if (std::filesystem::exists(events_file)) {
      if (auto pairs = read_final_events_file(
              work_directory_, consensus_value.proposal_header.events_hash);
          !pairs) {
        error("canonical events file for tip height {} is unreadable: {}",
              tip_height, pairs.error());
        co_return EventError{EventErrorCode::kInternal};
      }
      promotion_needed = true;
    }
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
    if (tip_height < 0 ||
        (canonical_boundary &&
         (tip_height != canonical_boundary->height ||
          consensus_value.prev_hash != canonical_boundary->consensus_hash)) ||
        (previous_record &&
         (previous_record->block_height == INT64_MAX ||
          tip_height != previous_record->block_height + 1 ||
          consensus_value.prev_hash != previous_record->consensus_hash))) {
      error("recovered events file does not append to validated outbox "
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
          error("current outbox origin does not match recovered block");
          co_return EventError{EventErrorCode::kInternal};
        }
      }
      else {
        error("recovered block promotion requires an established current "
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

  // Committed records reference their <event_root>.events files, so every
  // validated record takes a permanent claim on its file's name. This is what
  // stops a later live block that happens to share a root (every empty block
  // does) from unlinking a record's payload when it is abandoned. Records
  // committed after this point take their claim in commit_block.
  {
    x23::CoroGuard g;
    co_await g(lock_);
    for (const auto &record : *records) {
      retain_events_file(record.event_root);
    }
  }

  // Mutation phase. Every rejection above preserves events files and committed
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
      error("promoted recovered block is not the committed outbox tip");
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
          progress.last_consensus_hash != progress.origin.first_prev_hash) {
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
          progress.last_consensus_hash == progress.origin.first_prev_hash;
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
  // The loaded record already validated both consensus roots and the header's
  // consensus_hash, so preserve that durable identity instead of hashing the
  // consensus value again.
  IndexerCommittedBlock block{
      .payload = std::move(loaded->payload),
      .identity =
          IndexerCommittedBlockIdentity{
              .consensus_hash = loaded->info.consensus_hash,
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
    co_await finish_block_operation(block_id, nullptr, false);
    co_return EventError{EventErrorCode::kBadBlockID};
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
EventRPCContext::retain_events_file(SHA256Hash const &hash)
{
  ++events_file_refs_[hash];
}

void
EventRPCContext::release_events_file(SHA256Hash const &hash)
{
  auto it = events_file_refs_.find(hash);
  CHECK(it != events_file_refs_.end());
  CHECK_GT(it->second, 0U);
  if (--it->second > 0) {
    return;
  }
  events_file_refs_.erase(it);
  auto name = EventBlockContext::final_events_filename(work_directory_, hash);
  if (std::remove(name.c_str()) != 0) {
    warn("failed to remove events file '{}': {}", name, strerror(errno));
  }
}

void
EventRPCContext::gc_context_internal(
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator it)
{
  // the block must already have been shutdown by this point,
  // and should have no outstanding references
  //
  // Drop this block's claim on its events file before the context goes away.
  // By here the block is settled: a committed block has already published its
  // durable outbox record (commit_block publishes before it finishes the block
  // operation), and an abandoned block lost to a sibling that committed.  What
  // the file's name may still be needed for is some *other* live block that
  // shares the same event root, which is what the refcount tracks.
  if (it->second.get().has_root_hash()) {
    release_events_file(it->second.get().root_hash());
  }

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
