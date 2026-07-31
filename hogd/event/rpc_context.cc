#include "hogd/event/rpc_context.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/indexer_outbox.h"
#include "hogd/event/status.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "network.hh"
#include "proposal.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

namespace detail {

// Generated XDR strings make clang-tidy infer an infeasible throwing copy from
// the implicit move operations used by the delivery queue.
// NOLINTNEXTLINE(bugprone-exception-escape,misc-use-internal-linkage)
struct IndexerDeliveryRecord {
  IndexerOutboxOrigin origin;
  BlockHeight block_height;
};

// This type is part of IndexerDeliveryState, whose declaration is in the
// header. NOLINTNEXTLINE(misc-use-internal-linkage)
struct RegisteredIndexer {
  UPID generation;
  indexer_event_client_t client;
  std::shared_ptr<x23::CoroLock> lane;
};

struct IndexerDeliveryState {
  std::mutex mutex;
  std::condition_variable drained;
  std::optional<RegisteredIndexer> registered;
  std::optional<UPID> durable_generation;
  std::vector<std::pair<uint64_t, indexer_event_client_t>> active_clients;
  std::deque<IndexerDeliveryRecord> queue;
  uint64_t next_operation_id = 0;
  size_t active_operations = 0;
  bool worker_running = false;
  bool stopping = false;
  std::shared_ptr<void> worker_owner;
  std::function<void()> worker_empty_test_hook;
  std::function<void()> delivery_succeeded_test_hook;
  const IndexerOutboxStore *outbox_store = nullptr;
};

} // namespace detail

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
                  const ProposalTxs &proposal_txs)
{
  return candidate.block_id == block_id &&
         candidate.block_height == block_height &&
         xdr_equal(candidate.proposal_txs, proposal_txs);
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

// Releases a reference acquired by try_acquire_indexer_snapshot(). The last
// release of a retired snapshot destroys it (and with it the ClientRefs of
// any since-removed indexers).
void
release_indexer_snapshot(indexer_snapshot_t *snapshot)
{
  if (snapshot->release_reference()) {
    delete snapshot;
  }
}

// Send one report to a single indexer and free its in-flight slot when the
// send settles (reply or RPC-layer throw), independently of the other
// indexers. A per-indexer catch keeps one indexer's throw from disturbing the
// others; bad_alloc and other non-RPC throws are genuine bugs and still
// terminate (cf. EventServerProxy::emit_event's #2857 containment).
x23::Async<void>
relay_one_tx_failure(indexer_event_client_t &client,
                     std::atomic<size_t> &in_flight,
                     SignedTransactionHash tx_hash, TxFailureDiagnostic diag)
{
  using enum std::memory_order;
  // [client] and [in_flight] reference the snapshot, which the spawning relay
  // keeps alive (via its snapshot reference) for longer than this coroutine.
  x23::Defer dec{[&in_flight] { in_flight.fetch_sub(1, relaxed); }};
  try {
    co_await client.indexer_event_report_tx_failure_v2(tx_hash, diag);
  } catch (x23::XdrException const &e) {
    warn("dropping tx-failure report to an indexer: {}", e.what());
  }
  co_return;
}

// Fan out one failure report to every indexer concurrently (dispatch all,
// then await). Admission is bounded PER INDEXER: a report is dropped for an
// indexer already at kMaxInFlightTxFailureRelaysPerIndexer in-flight relays,
// but still relayed to the others -- so one connected-but-unresponsive
// indexer (whose relays never settle and never free their slots) cannot
// starve healthy indexers of diagnostics (#3124). The count is bumped here
// and dropped by relay_one_tx_failure when that indexer's send settles.
x23::Async<void>
relay_tx_failure(IndexerSnapshot &snapshot, SignedTransactionHash tx_hash,
                 TxFailureDiagnostic diag)
{
  // clients[i] and tx_failure_in_flight[i] describe the same indexer (built
  // index-aligned at publish); walk them together.
  CHECK(snapshot.clients.size() == snapshot.tx_failure_in_flight.size());
  std::vector<x23::Async<void>> handles;
  handles.reserve(snapshot.clients.size());
  for (size_t i = 0; i < snapshot.clients.size(); ++i) {
    std::atomic<size_t> &in_flight = *snapshot.tx_failure_in_flight[i];
    if (in_flight.fetch_add(1, std::memory_order_relaxed) >=
        EventRPCContext::kMaxInFlightTxFailureRelaysPerIndexer) {
      in_flight.fetch_sub(1, std::memory_order_relaxed);
      warn_every_n_sec(5.0F, "tx-failure relay dropped: indexer at cap");
      continue;
    }
    handles.push_back(
        relay_one_tx_failure(snapshot.clients[i], in_flight, tx_hash, diag));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
  co_return;
}

bool
apply_succeeded(const IndexerBlockApplyResult &result)
{
  return result.outcome() == IndexerBlockApplyOutcome::IndexerBlockApplied ||
         result.outcome() == IndexerBlockApplyOutcome::IndexerBlockDuplicate;
}

bool
lineage_admits(const std::optional<UPID> &durable_generation,
               const IndexerCardV2 &card)
{
  if (!durable_generation) {
    return !card.predecessor_generation;
  }
  return card.generation == *durable_generation ||
         (card.predecessor_generation &&
          *card.predecessor_generation == *durable_generation);
}

// Relaunching creates a detached coroutine that immediately suspends on bg(),
// so this does not grow the current coroutine's stack.
x23::Detached
// NOLINTNEXTLINE(misc-no-recursion)
drain_indexer_delivery_queue(
    std::shared_ptr<detail::IndexerDeliveryState> state,
    std::shared_ptr<void> worker_owner)
{
  co_await default_threadpool().bg();
  auto replacement_candidate = std::make_shared<char>();
  std::shared_ptr<void> replacement_owner;
  {
    x23::Defer finish_worker{[state, worker_owner, replacement_candidate,
                              &replacement_owner] {
      std::lock_guard g(state->mutex);
      CHECK_GT(state->active_operations, 0UL);
      --state->active_operations;
      if (state->worker_owner == worker_owner) {
        state->worker_running = false;
        state->worker_owner.reset();
        if (!state->stopping && state->registered && !state->queue.empty()) {
          replacement_owner = replacement_candidate;
          state->worker_owner = replacement_owner;
          state->worker_running = true;
          ++state->active_operations;
        }
      }
      state->drained.notify_all();
    }};
    while (true) {
      std::optional<detail::IndexerDeliveryRecord> record;
      std::optional<detail::RegisteredIndexer> registered;
      {
        std::lock_guard g(state->mutex);
        if (state->stopping || state->queue.empty()) {
          if (!state->stopping && state->worker_empty_test_hook) {
            auto hook = std::move(state->worker_empty_test_hook);
            hook();
          }
          break;
        }
        record = state->queue.front();
        registered = state->registered;
      }
      if (!registered) {
        break;
      }

      CHECK_PTR(state->outbox_store);
      auto payload = state->outbox_store->load_committed(record->block_height);
      if (!payload) {
        panic("failed to reload committed indexer delivery {}: {}",
              record->block_height, payload.error());
      }
      auto block = make_indexer_committed_block(*payload);
      if (!block) {
        panic("failed to rebuild committed indexer delivery {}: {}",
              record->block_height, block.error());
      }

      x23::CoroGuard generation_lane;
      co_await generation_lane(*registered->lane);
      uint64_t rpc_operation_id = 0;
      {
        std::lock_guard g(state->mutex);
        if (state->stopping) {
          break;
        }
        registered = state->registered;
        if (!registered) {
          continue;
        }
        // Registration replay owns the same generation lane and may already
        // have acknowledged and erased this queued record while the worker was
        // loading it.
        if (state->queue.empty() ||
            state->queue.front().block_height != record->block_height) {
          continue;
        }
        rpc_operation_id = ++state->next_operation_id;
        state->active_clients.emplace_back(rpc_operation_id,
                                           registered->client);
      }
      x23::Defer finish_rpc{[state, rpc_operation_id] {
        std::lock_guard g(state->mutex);
        std::erase_if(state->active_clients, [&](const auto &entry) {
          return entry.first == rpc_operation_id;
        });
      }};
      try {
        IndexerBlockApplyResult result =
            co_await registered->client.indexer_event_apply_committed_block(
                record->origin, *block);
        if (!apply_succeeded(result) ||
            result.progress().origin != record->origin ||
            result.progress().contiguous_height != record->block_height ||
            result.progress().last_consensus_hash !=
                block->identity.consensus_hash ||
            result.progress().last_payload_digest !=
                block->identity.payload_digest) {
          warn("indexer rejected committed block {} with outcome {}",
               record->block_height, result.outcome());
          panic("fatal committed indexer delivery rejection");
        }
        std::function<void()> delivery_succeeded_test_hook;
        {
          std::lock_guard g(state->mutex);
          delivery_succeeded_test_hook =
              std::move(state->delivery_succeeded_test_hook);
        }
        if (delivery_succeeded_test_hook) {
          delivery_succeeded_test_hook();
        }
        {
          std::lock_guard g(state->mutex);
          if (state->stopping) {
            break;
          }
          CHECK(!state->queue.empty());
          CHECK_EQ(state->queue.front().block_height, record->block_height);
          state->queue.pop_front();
        }
      } catch (const x23::XdrException &e) {
        {
          std::lock_guard g(state->mutex);
          if (state->stopping) {
            break;
          }
        }
        panic("fatal committed indexer delivery failure at height {}: {}",
              record->block_height, e.what());
      }
    }
  }
  if (replacement_owner) {
    drain_indexer_delivery_queue(state, std::move(replacement_owner));
  }
}

} // namespace

EventRPCContext::EventRPCContext(std::string_view work_directory)
  : indexer_delivery_state_(std::make_shared<detail::IndexerDeliveryState>()),
    work_directory_(work_directory),
    indexer_outbox_store_(open_indexer_outbox_store(work_directory))
{
  auto slot = indexer_outbox_store_.load_indexer_slot();
  if (!slot) {
    throw std::runtime_error(
        std::format("failed to load durable indexer slot: {}", slot.error()));
  }
  if (*slot) {
    indexer_delivery_state_->durable_generation = (**slot).generation;
  }
  indexer_delivery_state_->outbox_store = &indexer_outbox_store_;
}

EventRPCContext::~EventRPCContext()
{
  auto state = indexer_delivery_state_;
  std::optional<indexer_event_client_t> registered_client;
  std::vector<std::pair<uint64_t, indexer_event_client_t>> active_clients;
  {
    std::lock_guard g(state->mutex);
    state->stopping = true;
    if (state->registered) {
      registered_client.emplace(std::move(state->registered->client));
      state->registered.reset();
    }
    active_clients.swap(state->active_clients);
    state->queue.clear();
  }
  // RpcStream::fail closes the socket and resolves every pending call. This is
  // the available x23 cancellation primitive; no deadline API exists.
  if (registered_client) {
    registered_client->__send_call.get()->fail();
  }
  for (auto &[operation_id, client] : active_clients) {
    std::ignore = operation_id;
    client.__send_call.get()->fail();
  }
  {
    std::unique_lock lock(state->mutex);
    state->drained.wait(lock, [&] { return state->active_operations == 0; });
  }

  // Best-effort diagnostic relays own only their refcounted snapshot and may
  // finish after this context; they never touch the delivery state above.
  if (indexers_snapshot_->initiate_shutdown()) {
    delete indexers_snapshot_;
  }
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
                             ProposalTxs proposal_txs)
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
    auto candidate = indexer_outbox_store_.load_candidate(block_id);
    if (!candidate) {
      error("failed to load candidate indexer outbox record for block {}: {}",
            block_id, candidate.error());
      co_await finish_block_operation(block_id, nullptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!candidate_matches(*candidate, block_id, block_height, proposal_txs)) {
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
        context->get().snapshot_indexer_outbox_candidate(block_height, out_hash,
                                                         std::move(
                                                             proposal_txs));
    if (auto published = indexer_outbox_store_.publish_candidate(candidate);
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
  auto promotion = find_canonical_candidate(consensus_value);
  if (!promotion) {
    error("failed to identify canonical candidate for height {}: {}",
          block_height, promotion.error());
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  auto committed_exists = indexer_outbox_store_.has_committed(block_height);
  if (!committed_exists) {
    error("failed to inspect committed indexer outbox record at height {}: {}",
          block_height, committed_exists.error());
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  IndexerOutboxCommittedPayload committed;
  if (*committed_exists) {
    auto loaded = indexer_outbox_store_.load_committed(block_height);
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
    if (*promotion && !xdr_equal((*promotion)->committed, *loaded)) {
      error("candidate full payload conflicts with committed record at height "
            "{}",
            block_height);
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    committed = std::move(*loaded);
  }
  else {
    if (!*promotion) {
      error("no canonical candidate exists for committed height {}",
            block_height);
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
    committed = std::move((*promotion)->committed);
    if (auto published = indexer_outbox_store_.publish_committed(committed);
        !published) {
      error("failed to publish committed indexer outbox record for block {}: "
            "{}",
            block_id, published.error());
      co_await finish_block_operation(block_id, context_ptr, false);
      co_return EventError{EventErrorCode::kInternal};
    }
  }
  if (auto removed = remove_resolved_candidates(block_height); !removed) {
    error("failed to remove resolved candidate indexer outbox records through "
          "height {}: {}",
          block_height, removed.error());
    co_await finish_block_operation(block_id, context_ptr, false);
    co_return EventError{EventErrorCode::kInternal};
  }
  outbox_guard.reset();

  info("EventRPCContext::commit_block: block_id={} done, gc", block_id);
  co_await finish_block_operation(block_id, context_ptr,
                                  context_ptr != nullptr);
  co_return block_height;
}

x23::Async<EventStatus<BlockHeight>>
EventRPCContext::confirm_committed_block(ConsensusValue consensus_value)
{
  if (!outbox_reconciled_.load(std::memory_order_acquire)) {
    co_return EventError{EventErrorCode::kInternal};
  }

  const BlockHeight block_height = consensus_value.proposal_header.block_height;
  x23::CoroGuard outbox_guard;
  co_await outbox_guard(outbox_commit_lock_);
  if (!admissible_indexer_tail_) {
    error("cannot confirm indexer delivery without a reconciled tail");
    co_return EventError{EventErrorCode::kInternal};
  }
  auto committed = indexer_outbox_store_.load_committed(block_height);
  if (!committed) {
    error("cannot confirm missing committed indexer outbox height {}: {}",
          block_height, committed.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  if (!xdr_equal(committed->consensus_value, consensus_value)) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  if (block_height < *admissible_indexer_tail_) {
    error("stale indexer delivery confirmation at height {} below tail {}",
          block_height, *admissible_indexer_tail_);
    co_return EventError{EventErrorCode::kInternal};
  }
  if (block_height == *admissible_indexer_tail_) {
    co_return block_height;
  }
  if (*admissible_indexer_tail_ == INT64_MAX ||
      block_height != *admissible_indexer_tail_ + 1) {
    error("indexer delivery confirmation skipped from height {} to {}",
          *admissible_indexer_tail_, block_height);
    co_return EventError{EventErrorCode::kInternal};
  }
  auto origin = indexer_outbox_store_.load_current_epoch_origin();
  if (!origin || !*origin) {
    error("committed indexer outbox has no current epoch origin");
    co_return EventError{EventErrorCode::kInternal};
  }

  admissible_indexer_tail_ = block_height;
  auto delivery_worker_owner =
      enqueue_indexer_delivery((**origin).origin, *committed);
  outbox_guard.reset();
  if (delivery_worker_owner) {
    start_indexer_delivery_worker(std::move(delivery_worker_owner));
  }
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

  // Validation phase. No durable state is changed until every retained origin
  // and committed record, the protocol sequence, candidate promotion boundary,
  // and scheduler-tip relationship have passed.
  auto records = indexer_outbox_store_.enumerate_committed_records_raw();
  if (!records) {
    error("failed to enumerate committed indexer outbox records: {}",
          records.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  auto candidates = indexer_outbox_store_.enumerate_candidate_records_raw();
  if (!candidates) {
    error("failed to enumerate candidate indexer outbox records: {}",
          candidates.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  auto origins = indexer_outbox_store_.enumerate_epoch_origins_raw();
  if (!origins) {
    error("failed to enumerate indexer outbox epoch origins: {}",
          origins.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  auto current_origin = indexer_outbox_store_.load_current_epoch_origin();
  if (!current_origin) {
    error("failed to load current indexer outbox epoch origin: {}",
          current_origin.error());
    co_return EventError{EventErrorCode::kInternal};
  }

  std::unordered_map<std::string, IndexerOutboxEpochOriginInfo>
      origins_by_protocol;
  std::optional<size_t> current_origin_index;
  for (size_t i = 0; i < origins->size(); ++i) {
    const auto &origin_info = (*origins)[i];
    if (origin_info.epoch_sequence != i) {
      error("indexer outbox epoch sequence is discontinuous at {}",
            origin_info.epoch_sequence);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!origins_by_protocol.emplace(origin_info.protocol, origin_info)
             .second) {
      error("duplicate indexer outbox epoch origin for protocol {}",
            origin_info.protocol);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (origin_info.protocol == current_protocol) {
      current_origin_index = i;
    }
  }
  if (current_origin_index && *current_origin_index + 1 != origins->size()) {
    error("archival indexer outbox epoch appears after current epoch");
    co_return EventError{EventErrorCode::kInternal};
  }
  if (current_origin->has_value() != current_origin_index.has_value()) {
    error("current indexer outbox epoch origin metadata is inconsistent");
    co_return EventError{EventErrorCode::kInternal};
  }
  if (*current_origin) {
    const auto &current_origin_info = (*origins)[*current_origin_index];
    if ((*current_origin)->epoch_sequence !=
            current_origin_info.epoch_sequence ||
        (*current_origin)->origin.first_height !=
            current_origin_info.first_height ||
        (*current_origin)->origin.first_prev_hash !=
            current_origin_info.first_prev_hash) {
      error(
          "current indexer outbox epoch origin payload does not match header");
      co_return EventError{EventErrorCode::kInternal};
    }
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
  auto promotion = find_canonical_candidate(consensus_value);
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

  bool saw_current_epoch = false;
  BlockHeight last_current_height = -1;
  std::optional<IndexerOutboxCommittedRecordInfo> previous_current;
  std::optional<IndexerOutboxCommittedRecordInfo> previous_record;
  size_t record_index = 0;
  std::optional<IndexerOutboxCanonicalBoundary> canonical_boundary;
  bool boundary_exhausted = false;
  const BlockHeight max_allowed_height =
      tip_height == INT64_MAX ? INT64_MAX : tip_height + 1;
  for (const auto &origin : *origins) {
    if (boundary_exhausted) {
      error("indexer outbox epoch {} follows exhausted canonical tail",
            origin.protocol);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (canonical_boundary &&
        (origin.first_height != canonical_boundary->height ||
         origin.first_prev_hash != canonical_boundary->consensus_hash)) {
      error("indexer outbox epoch {} does not continue canonical tail",
            origin.protocol);
      co_return EventError{EventErrorCode::kInternal};
    }
    if (!canonical_boundary) {
      canonical_boundary = IndexerOutboxCanonicalBoundary{
          .height = origin.first_height,
          .consensus_hash = origin.first_prev_hash,
      };
    }

    bool first_record = true;
    while (record_index < records->size() &&
           (*records)[record_index].protocol == origin.protocol) {
      const auto &record = (*records)[record_index];
      if (record.block_height > max_allowed_height) {
        error("committed indexer outbox height {} is ahead of canonical tip {}",
              record.block_height, tip_height);
        co_return EventError{EventErrorCode::kInternal};
      }
      if ((first_record && (record.block_height != origin.first_height ||
                            record.prev_hash != origin.first_prev_hash)) ||
          (!first_record &&
           (previous_record->block_height == INT64_MAX ||
            record.block_height != previous_record->block_height + 1 ||
            record.prev_hash != previous_record->consensus_hash))) {
        error("committed outbox chain is disconnected at {}",
              record.block_height);
        co_return EventError{EventErrorCode::kInternal};
      }

      if (record.protocol == current_protocol) {
        auto committed =
            indexer_outbox_store_.load_committed(record.block_height);
        if (!committed) {
          error("failed to validate current committed outbox record at {}: {}",
                record.block_height, committed.error());
          co_return EventError{EventErrorCode::kInternal};
        }
        if (sha256(committed->consensus_value) != record.consensus_hash ||
            committed->consensus_value.prev_hash != record.prev_hash) {
          error("current committed outbox header/payload mismatch at {}",
                record.block_height);
          co_return EventError{EventErrorCode::kInternal};
        }
        saw_current_epoch = true;
        last_current_height = record.block_height;
        previous_current = record;
      }

      previous_record = record;
      if (record.block_height == INT64_MAX) {
        canonical_boundary.reset();
        boundary_exhausted = true;
      }
      else {
        canonical_boundary = IndexerOutboxCanonicalBoundary{
            .height = record.block_height + 1,
            .consensus_hash = record.consensus_hash,
        };
      }
      first_record = false;
      ++record_index;
    }
  }
  if (record_index != records->size()) {
    error("committed outbox records do not follow epoch origin order");
    co_return EventError{EventErrorCode::kInternal};
  }

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
    else if (last_current_height == tip_height + 1) {
      if (previous_current->prev_hash != tip_hash) {
        error("single outbox-ahead record does not link to scheduler tip");
        co_return EventError{EventErrorCode::kInternal};
      }
      info("event outbox is one committed block ahead at height {}; awaiting "
           "scheduler catchup",
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
        (previous_record && ((*current_origin)->origin.first_height !=
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
    auto initialized = indexer_outbox_store_.initialize_current_epoch_origin(
        planned_origin->first, planned_origin->second);
    if (!initialized) {
      error("failed to initialize current indexer outbox epoch origin: {}",
            initialized.error());
      co_return EventError{EventErrorCode::kInternal};
    }
    current_origin = indexer_outbox_store_.load_current_epoch_origin();
    if (!current_origin || !*current_origin) {
      error("current indexer outbox epoch origin was not durably initialized");
      co_return EventError{EventErrorCode::kInternal};
    }
  }

  if (promotion_needed) {
    auto promoted = co_await commit_block(block_id, consensus_value);
    if (!promoted) {
      co_return promoted.error();
    }
    auto committed = indexer_outbox_store_.load_committed(tip_height);
    if (!committed || sha256(committed->consensus_value) != tip_hash ||
        committed->consensus_value.prev_hash != consensus_value.prev_hash) {
      error("promoted retained candidate did not produce the validated outbox "
            "boundary");
      co_return EventError{EventErrorCode::kInternal};
    }
    auto latest = indexer_outbox_store_.latest_committed_record_raw();
    if (!latest || !*latest || (*latest)->block_height != tip_height ||
        (*latest)->consensus_hash != tip_hash) {
      error("promoted retained candidate is not the committed outbox tip");
      co_return EventError{EventErrorCode::kInternal};
    }
    previous_record = std::move(**latest);
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
  auto durable_tip = indexer_outbox_store_.latest_committed_record_raw();
  if (!durable_tip) {
    error("failed to re-read committed indexer outbox tip: {}",
          durable_tip.error());
    co_return EventError{EventErrorCode::kInternal};
  }
  const auto same_tip = [](const auto &lhs, const auto &rhs) {
    if (lhs.has_value() != rhs.has_value()) {
      return false;
    }
    return !lhs || (lhs->block_height == rhs->block_height &&
                    lhs->protocol == rhs->protocol &&
                    lhs->payload_byte_length == rhs->payload_byte_length &&
                    lhs->payload_digest == rhs->payload_digest &&
                    lhs->consensus_hash == rhs->consensus_hash &&
                    lhs->prev_hash == rhs->prev_hash);
  };
  if (!same_tip(*durable_tip, previous_record)) {
    error("committed indexer outbox advanced during reconciliation");
    co_return EventError{EventErrorCode::kInternal};
  }

  indexer_outbox_store_.cache_validated_committed_append_tip(
      previous_record, *current_origin, canonical_boundary);

  if (previous_record) {
    auto removed = remove_resolved_candidates(previous_record->block_height);
    if (!removed) {
      error("failed to clean reconciled candidates through height {}: {}",
            previous_record->block_height, removed.error());
      co_return EventError{EventErrorCode::kInternal};
    }
  }

  admissible_indexer_tail_ = tip_height;
  outbox_reconciled_.store(true, std::memory_order_release);
  co_return EventStatus<void>();
}

x23::Async<EventIndexerRegistrationOutcome>
EventRPCContext::register_indexer(indexer_event_client_t indexer,
                                  const IndexerCardV2 &card)
{
  using enum EventIndexerRegistrationOutcome;
  if (!outbox_reconciled_.load(std::memory_order_acquire)) {
    co_return EventRegistrationOriginUnavailable;
  }

  x23::CoroGuard registration_guard;
  co_await registration_guard(indexer_registration_lock_);

  auto state = indexer_delivery_state_;
  uint64_t operation_id = 0;
  std::shared_ptr<x23::CoroLock> generation_lane;
  {
    std::lock_guard g(state->mutex);
    if (state->stopping) {
      co_return EventRegistrationOriginUnavailable;
    }
    if (state->registered) {
      if (state->registered->generation != card.generation) {
        co_return EventRegistrationStaleGeneration;
      }
      generation_lane = state->registered->lane;
    }
    else if (!lineage_admits(state->durable_generation, card)) {
      co_return EventRegistrationStaleGeneration;
    }
    else {
      generation_lane = std::make_shared<x23::CoroLock>();
    }
    operation_id = ++state->next_operation_id;
    ++state->active_operations;
    state->active_clients.emplace_back(operation_id, indexer);
  }
  x23::Defer finish_registration{[state, operation_id] {
    std::lock_guard g(state->mutex);
    std::erase_if(state->active_clients, [&](const auto &entry) {
      return entry.first == operation_id;
    });
    CHECK_GT(state->active_operations, 0UL);
    --state->active_operations;
    state->drained.notify_all();
  }};
  x23::CoroGuard generation_guard;
  co_await generation_guard(*generation_lane);

  IndexerProgress progress;
  try {
    progress = co_await indexer.indexer_event_get_progress();
  } catch (const x23::XdrCallFailed &e) {
    warn("indexer progress RPC failed during registration: {}", e.what());
    co_return EventRegistrationProgressConflict;
  } catch (const x23::XdrBadInput &e) {
    warn("indexer returned malformed progress during registration: {}",
         e.what());
    co_return EventRegistrationProgressConflict;
  } catch (const x23::XdrLengthError &e) {
    warn("indexer returned malformed progress during registration: {}",
         e.what());
    co_return EventRegistrationProgressConflict;
  } catch (const x23::XdrException &e) {
    warn("failed to read indexer progress during registration: {}", e.what());
    co_return EventRegistrationConnectFailed;
  }

  std::optional<IndexerInitializedProgress> initialized;
  while (true) {
    std::optional<IndexerOutboxEpochOrigin> current_origin;
    std::vector<IndexerOutboxCommittedRecordInfo> records;
    std::vector<IndexerOutboxEpochOriginInfo> origins;
    std::optional<BlockHeight> admissible_tail;
    {
      x23::CoroGuard outbox_guard;
      co_await outbox_guard(outbox_commit_lock_);
      auto loaded_origin = indexer_outbox_store_.load_current_epoch_origin();
      auto loaded_records =
          indexer_outbox_store_.enumerate_committed_records_raw();
      auto loaded_origins = indexer_outbox_store_.enumerate_epoch_origins_raw();
      if (!loaded_origin || !*loaded_origin || !loaded_records ||
          !loaded_origins || !admissible_indexer_tail_) {
        warn("indexer registration cannot inspect reconciled outbox state");
        co_return EventRegistrationOriginUnavailable;
      }
      current_origin = std::move(**loaded_origin);
      records = std::move(*loaded_records);
      origins = std::move(*loaded_origins);
      admissible_tail = admissible_indexer_tail_;
    }

    const auto validate_progress =
        [&](const IndexerInitializedProgress &value) {
          const auto origin_it =
              std::ranges::find_if(origins, [&](const auto &info) {
                return info.protocol ==
                           std::string_view(value.origin.protocol) &&
                       info.epoch_sequence == value.origin.epoch_sequence &&
                       info.first_height == value.origin.first_height &&
                       info.first_prev_hash == value.origin.first_prev_hash;
              });
          if (origin_it == origins.end()) {
            warn("indexer registration reports an unknown outbox origin");
            return false;
          }
          const auto boundary =
              std::ranges::find_if(records, [&](const auto &record) {
                return record.block_height == value.contiguous_height;
              });
          if (value.contiguous_height == value.origin.first_height - 1) {
            const bool genesis_empty =
                value.contiguous_height == 0 && boundary == records.end();
            if (value.last_consensus_hash != value.origin.first_prev_hash ||
                (boundary != records.end() &&
                 (boundary->consensus_hash != value.last_consensus_hash ||
                  boundary->payload_digest != value.last_payload_digest)) ||
                (boundary == records.end() &&
                 (!genesis_empty ||
                  value.last_payload_digest != SHA256Hash{}))) {
              warn("indexer registration reports an invalid epoch boundary");
              return false;
            }
          }
          else if (boundary == records.end() ||
                   boundary->consensus_hash != value.last_consensus_hash ||
                   boundary->payload_digest != value.last_payload_digest) {
            warn("indexer registration boundary conflicts with durable outbox");
            return false;
          }
          if (value.contiguous_height > *admissible_tail) {
            warn("indexer registration progress is above the admissible outbox "
                 "tail");
            return false;
          }
          return true;
        };

    if (!initialized &&
        progress.state() == IndexerProgressState::IndexerProgressInitialized) {
      if (!validate_progress(progress.initialized())) {
        co_return EventRegistrationProgressConflict;
      }
      initialized = progress.initialized();
    }

    if (!initialized || initialized->origin != current_origin->origin) {
      IndexerEpochResult result;
      try {
        result = co_await indexer.indexer_event_initialize_epoch(
            current_origin->origin);
      } catch (const x23::XdrCallFailed &e) {
        warn("indexer rejected epoch initialization RPC: {}", e.what());
        co_return EventRegistrationReplayFailed;
      } catch (const x23::XdrBadInput &e) {
        warn("indexer returned malformed epoch initialization reply: {}",
             e.what());
        co_return EventRegistrationReplayFailed;
      } catch (const x23::XdrLengthError &e) {
        warn("indexer returned malformed epoch initialization reply: {}",
             e.what());
        co_return EventRegistrationReplayFailed;
      } catch (const x23::XdrException &e) {
        warn("indexer epoch initialization transport failed: {}", e.what());
        co_return EventRegistrationConnectFailed;
      }
      if (result.outcome() != IndexerEpochOutcome::IndexerEpochInitialized &&
          result.outcome() != IndexerEpochOutcome::IndexerEpochUnchanged) {
        warn("indexer rejected outbox epoch with outcome {}", result.outcome());
        co_return EventRegistrationProgressConflict;
      }
      initialized = result.progress();
      if (!validate_progress(*initialized)) {
        co_return EventRegistrationProgressConflict;
      }
    }

    CHECK(initialized.has_value());
    std::optional<BlockHeight> next_height;
    if (initialized->contiguous_height != INT64_MAX) {
      next_height = initialized->contiguous_height + 1;
    }
    for (const auto &record : records) {
      if (record.protocol !=
              std::string_view(current_origin->origin.protocol) ||
          record.block_height > *admissible_tail || !next_height ||
          record.block_height < *next_height) {
        continue;
      }
      if (record.block_height != *next_height) {
        warn("indexer registration replay found an outbox gap at height {}",
             *next_height);
        co_return EventRegistrationReplayFailed;
      }
      auto payload = indexer_outbox_store_.load_committed(record.block_height);
      if (!payload) {
        warn("failed to load indexer replay record {}: {}", record.block_height,
             payload.error());
        co_return EventRegistrationReplayFailed;
      }
      auto block = make_indexer_committed_block(*payload);
      if (!block || block->identity.consensus_hash != record.consensus_hash ||
          block->identity.payload_digest != record.payload_digest) {
        warn("indexer replay record {} failed identity validation",
             record.block_height);
        co_return EventRegistrationReplayFailed;
      }
      IndexerBlockApplyResult result;
      try {
        result = co_await indexer.indexer_event_apply_committed_block(
            current_origin->origin, *block);
      } catch (const x23::XdrCallFailed &e) {
        warn("indexer rejected replay RPC at height {}: {}",
             record.block_height, e.what());
        co_return EventRegistrationReplayFailed;
      } catch (const x23::XdrBadInput &e) {
        warn("indexer returned malformed replay reply at height {}: {}",
             record.block_height, e.what());
        co_return EventRegistrationReplayFailed;
      } catch (const x23::XdrLengthError &e) {
        warn("indexer returned malformed replay reply at height {}: {}",
             record.block_height, e.what());
        co_return EventRegistrationReplayFailed;
      } catch (const x23::XdrException &e) {
        warn("indexer replay transport failed at height {}: {}",
             record.block_height, e.what());
        co_return EventRegistrationConnectFailed;
      }
      if (!apply_succeeded(result) ||
          result.progress().origin != current_origin->origin ||
          result.progress().contiguous_height != record.block_height ||
          result.progress().last_consensus_hash != record.consensus_hash ||
          result.progress().last_payload_digest != record.payload_digest) {
        warn("indexer replay rejected height {} with outcome {}",
             record.block_height, result.outcome());
        co_return EventRegistrationReplayFailed;
      }
      initialized = result.progress();
      if (*next_height == INT64_MAX) {
        next_height.reset();
      }
      else {
        ++*next_height;
      }
    }

    {
      x23::CoroGuard outbox_guard;
      co_await outbox_guard(outbox_commit_lock_);
      auto latest = indexer_outbox_store_.latest_committed_record_raw();
      auto latest_origin = indexer_outbox_store_.load_current_epoch_origin();
      if (!latest || !latest_origin || !*latest_origin ||
          (**latest_origin).origin != current_origin->origin ||
          !admissible_indexer_tail_) {
        co_return EventRegistrationOriginUnavailable;
      }
      if (initialized->origin != (**latest_origin).origin ||
          initialized->contiguous_height < *admissible_indexer_tail_) {
        continue;
      }
      if (initialized->contiguous_height != *admissible_indexer_tail_) {
        co_return EventRegistrationProgressConflict;
      }

      std::lock_guard g(state->mutex);
      if (state->stopping) {
        co_return EventRegistrationOriginUnavailable;
      }
      if (!lineage_admits(state->durable_generation, card)) {
        co_return EventRegistrationStaleGeneration;
      }
      if (!state->durable_generation ||
          card.generation != *state->durable_generation) {
        auto persisted = indexer_outbox_store_.store_indexer_slot(
            EventIndexerSlot{.format_version = kEventIndexerSlotFormatVersion,
                             .generation = card.generation});
        if (!persisted) {
          warn("failed to persist indexer generation: {}", persisted.error());
          co_return EventRegistrationOriginUnavailable;
        }
        state->durable_generation = card.generation;
      }
      CHECK(!state->registered ||
            state->registered->generation == card.generation);
      state->registered = detail::RegisteredIndexer{
          .generation = card.generation,
          .client = indexer,
          .lane = generation_lane,
      };
      std::erase_if(state->queue, [&](const auto &queued) {
        return queued.block_height <= initialized->contiguous_height;
      });
      replace_diagnostic_indexer_locked(indexer);
    }
    co_return EventRegistrationAccepted;
  }
}

EventIndexerRegistrationOutcome
EventRPCContext::probe_indexer(const UPID &generation) const
{
  using enum EventIndexerRegistrationOutcome;
  auto state = indexer_delivery_state_;
  std::lock_guard g(state->mutex);
  if (state->stopping || !state->registered) {
    return EventRegistrationOriginUnavailable;
  }
  if (state->registered->generation != generation) {
    return EventRegistrationStaleGeneration;
  }
  return EventRegistrationAccepted;
}

std::shared_ptr<void>
EventRPCContext::enqueue_indexer_delivery(
    const IndexerOutboxOrigin &origin,
    const IndexerOutboxCommittedPayload &payload)
{
  auto state = indexer_delivery_state_;
  std::shared_ptr<void> worker_owner;
  {
    std::lock_guard g(state->mutex);
    if (!state->registered || state->stopping) {
      return nullptr;
    }
    state->queue.push_back(detail::IndexerDeliveryRecord{
        .origin = origin,
        .block_height = payload.consensus_value.proposal_header.block_height});
    if (!state->worker_running) {
      worker_owner = std::make_shared<char>();
      state->worker_owner = worker_owner;
      state->worker_running = true;
      ++state->active_operations;
    }
  }
  return worker_owner;
}

void
EventRPCContext::start_indexer_delivery_worker(
    std::shared_ptr<void> worker_owner)
{
  drain_indexer_delivery_queue(indexer_delivery_state_,
                               std::move(worker_owner));
}

void
EventRPCContext::set_delivery_worker_empty_test_hook(std::function<void()> hook)
{
  std::lock_guard g(indexer_delivery_state_->mutex);
  indexer_delivery_state_->worker_empty_test_hook = std::move(hook);
}

void
EventRPCContext::set_delivery_succeeded_test_hook(std::function<void()> hook)
{
  std::lock_guard g(indexer_delivery_state_->mutex);
  indexer_delivery_state_->delivery_succeeded_test_hook = std::move(hook);
}

void
EventRPCContext::replace_diagnostic_indexer_locked(
    const indexer_event_client_t &indexer)
{
  std::unique_lock g(indexers_mtx_);
  indexers_.clear();
  indexers_.emplace_back(next_indexer_id_++, indexer);
  publish_indexer_snapshot(g);
}

x23::Async<EventStatus<void>>
EventRPCContext::abandon_block(BlockID block_id)
{
  if (!co_await reserve_block_operation(block_id,
                                        BlockOperation::kAbandoning)) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  const auto clean_redundant_candidate = [&]() -> EventStatus<bool> {
    auto candidate_exists = indexer_outbox_store_.has_candidate(block_id);
    if (!candidate_exists) {
      error("failed to inspect candidate indexer outbox record for block {}: "
            "{}",
            block_id, candidate_exists.error());
      return EventError{EventErrorCode::kInternal};
    }
    if (!*candidate_exists) {
      return false;
    }
    auto candidate = indexer_outbox_store_.load_candidate(block_id);
    if (!candidate) {
      error("failed to load abandoned candidate {}: {}", block_id,
            candidate.error());
      return EventError{EventErrorCode::kInternal};
    }
    auto committed_exists =
        indexer_outbox_store_.has_committed(candidate->block_height);
    if (!committed_exists) {
      error("failed to inspect committed outbox record at height {}: {}",
            candidate->block_height, committed_exists.error());
      return EventError{EventErrorCode::kInternal};
    }
    if (!*committed_exists) {
      return true;
    }
    auto committed =
        indexer_outbox_store_.load_committed(candidate->block_height);
    if (!committed) {
      error("failed to validate committed outbox record at height {}: {}",
            candidate->block_height, committed.error());
      return EventError{EventErrorCode::kInternal};
    }
    if (auto removed = indexer_outbox_store_.remove_candidate(block_id);
        !removed) {
      error("failed to remove redundant abandoned candidate {}: {}", block_id,
            removed.error());
      return EventError{EventErrorCode::kInternal};
    }
    return true;
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
    auto candidate = clean_redundant_candidate();
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
  auto candidate = clean_redundant_candidate();
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

IndexerOutboxStatus<void>
EventRPCContext::remove_resolved_candidates(BlockHeight committed_height)
{
  auto candidates = indexer_outbox_store_.enumerate_candidate_records_raw();
  if (!candidates) {
    return std::unexpected(candidates.error());
  }

  for (const auto &candidate : *candidates) {
    if (candidate.block_height > committed_height) {
      continue;
    }
    if (candidate.protocol == std::string_view(version::kHogdProtocol())) {
      auto payload = indexer_outbox_store_.load_candidate(candidate.block_id);
      if (!payload) {
        return std::unexpected(
            std::format("failed to validate current candidate {}: {}",
                        candidate.block_id, payload.error()));
      }
    }
    if (auto removed =
            indexer_outbox_store_.remove_candidate(candidate.block_id);
        !removed) {
      return std::unexpected(std::format("failed to remove candidate {}: {}",
                                         candidate.block_id, removed.error()));
    }
  }
  return {};
}

IndexerOutboxStatus<std::optional<EventRPCContext::CandidatePromotion>>
EventRPCContext::find_canonical_candidate(
    const ConsensusValue &consensus_value) const
{
  auto records = indexer_outbox_store_.enumerate_candidate_records_raw();
  if (!records) {
    return std::unexpected(records.error());
  }
  const BlockHeight height = consensus_value.proposal_header.block_height;
  std::optional<CandidatePromotion> selected;
  x23::Buf selected_bytes;
  for (const auto &record : *records) {
    if (record.protocol != std::string_view(version::kHogdProtocol()) ||
        record.block_height != height ||
        record.event_root != consensus_value.proposal_header.events_hash ||
        record.proposal_txs_hash != consensus_value.proposal_txs_hash) {
      continue;
    }
    auto candidate = indexer_outbox_store_.load_candidate(record.block_id);
    if (!candidate) {
      return std::unexpected(std::format("failed to validate candidate {}: {}",
                                         record.block_id, candidate.error()));
    }
    IndexerOutboxCommittedPayload committed{
        .format_version = kIndexerOutboxFormatVersion,
        .consensus_value = consensus_value,
        .proposal_txs = candidate->proposal_txs,
        .event_logs = candidate->event_logs,
        .tx_bodies = candidate->tx_bodies,
    };
    if (auto valid = validate_indexer_committed_payload(committed); !valid) {
      return std::unexpected(std::format(
          "candidate {} cannot form canonical committed payload: {}",
          record.block_id, valid.error()));
    }
    x23::Buf bytes = x23::serialize(committed);
    if (selected && bytes != selected_bytes) {
      return std::unexpected(
          "multiple distinct candidates match the canonical consensus value");
    }
    if (!selected) {
      selected = CandidatePromotion{.block_id = record.block_id,
                                    .candidate = std::move(*candidate),
                                    .committed = std::move(committed)};
      selected_bytes = std::move(bytes);
    }
  }
  return selected;
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

indexer_snapshot_t *
EventRPCContext::try_acquire_indexer_snapshot()
{
  {
    std::scoped_lock g(cache_->mtx_);
    if (indexer_snapshot_t *snapshot = cache_->indexer_snapshot_) {
      // Cannot fail: publish_indexer_snapshot() clears this entry (under
      // this same mutex) before initiating the old snapshot's shutdown, and
      // try_acquire_reference() only fails once shutdown has begun -- so a
      // non-null entry read here always precedes its snapshot's shutdown.
      // CHECK rather than fall through: a failure means that ordering was
      // broken, which is a use-after-free waiting to happen elsewhere, not
      // a condition to paper over with the slow path.
      CHECK(snapshot->try_acquire_reference());
      return snapshot;
    }
  }

  std::scoped_lock g(indexers_mtx_);
  // Cannot fail: the snapshot's shutdown is only initiated after it is
  // unlinked from indexers_snapshot_, under indexers_mtx_.
  CHECK(indexers_snapshot_->try_acquire_reference());
  {
    std::scoped_lock cg(cache_->mtx_);
    cache_->indexer_snapshot_ = indexers_snapshot_;
  }
  return indexers_snapshot_;
}

void
EventRPCContext::publish_indexer_snapshot(
    const std::unique_lock<std::mutex> &indexers_ul)
{
  static_cast<void>(indexers_ul);
  assert(indexers_ul.owns_lock() && indexers_ul.mutex() == &indexers_mtx_);
  indexer_snapshot_t *old = indexers_snapshot_;
  // An empty indexer list publishes an empty-vector snapshot rather than
  // nullptr, so readers' per-thread caches stay valid (and hot) in the
  // no-indexer case; a cached nullptr always means "not cached". Each publish
  // gives every indexer a fresh in-flight counter (starting at zero). A
  // republish is rare (an indexer registering/deregistering), not
  // contract-driven, and the previous snapshot's counters keep bounding any
  // relays still in flight against it, so the per-indexer bound holds.
  IndexerSnapshot snapshot;
  snapshot.clients.reserve(indexers_.size());
  snapshot.tx_failure_in_flight.reserve(indexers_.size());
  for (auto &[id, client] : indexers_) {
    snapshot.clients.push_back(client);
    snapshot.tx_failure_in_flight.push_back(
        std::make_unique<std::atomic<size_t>>(0));
  }
  indexers_snapshot_ = new indexer_snapshot_t(std::move(snapshot));

  // Invalidate every thread's cached pointer before retiring [old] (same
  // sweep as gc_context_internal). A reader that acquired a reference under
  // its cache mutex before we sweep its cache is counted by
  // initiate_shutdown() and keeps [old] alive until it releases.
  auto thread_ul = cache_.lock_threads();
  cache_.for_each(thread_ul, [](ContextCache &cache) {
    std::scoped_lock g(cache.mtx_);
    cache.indexer_snapshot_ = nullptr;
  });

  if (old->initiate_shutdown()) {
    delete old;
  }
  // else: destroyed by the last release_indexer_snapshot().
}

uint64_t
EventRPCContext::add_indexer(indexer_event_client_t &&indexer)
{
  indexer_event_client_t delivery_client = indexer;
  std::unique_lock g(indexers_mtx_);
  uint64_t id = next_indexer_id_++;
  indexers_.emplace_back(id, std::move(indexer));
  publish_indexer_snapshot(g);
  g.unlock();

  UPID generation{};
  static_assert(sizeof(id) == generation.size());
  std::memcpy(generation.data(), &id, sizeof(id));
  std::lock_guard delivery_guard(indexer_delivery_state_->mutex);
  indexer_delivery_state_->registered =
      detail::RegisteredIndexer{.generation = generation,
                                .client = std::move(delivery_client),
                                .lane = std::make_shared<x23::CoroLock>()};
  return id;
}

bool
EventRPCContext::remove_indexer(uint64_t indexer_id)
{
  std::unique_lock g(indexers_mtx_);
  auto it = std::ranges::find(indexers_, indexer_id,
                              [](const auto &entry) { return entry.first; });
  if (it == indexers_.end()) {
    return false;
  }
  indexers_.erase(it);
  publish_indexer_snapshot(g);
  g.unlock();

  UPID generation{};
  static_assert(sizeof(indexer_id) == generation.size());
  std::memcpy(generation.data(), &indexer_id, sizeof(indexer_id));
  std::lock_guard delivery_guard(indexer_delivery_state_->mutex);
  if (indexer_delivery_state_->registered &&
      indexer_delivery_state_->registered->generation == generation) {
    indexer_delivery_state_->registered.reset();
  }
  return true;
}

x23::Async<void>
EventRPCContext::report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                               TxFailureDiagnostic diag)
{
  indexer_snapshot_t *snapshot = try_acquire_indexer_snapshot();
  // Defer (not a trailing call): a non-Xdr throw from the fan-out must still
  // release the reference, or a retired snapshot would never be destroyed.
  x23::Defer release{[snapshot] { release_indexer_snapshot(snapshot); }};
  if (snapshot->get().clients.empty()) {
    co_return;
  }
  co_await relay_tx_failure(snapshot->get(), tx_hash, diag);
  co_return;
}

void
EventRPCContext::spawn_tx_failure_relay(SignedTransactionHash tx_hash,
                                        TxFailureDiagnostic diag)
{
  // Fire-and-forget: the executor-facing report RPC hands the relay off to a
  // detached coroutine and returns immediately, so a slow indexer never holds
  // the executor's in-flight-report slot (or the RPC dispatch thread) hostage.
  //
  // The snapshot reference is acquired synchronously here, so the detached
  // coroutine owns everything it touches (the snapshot pins the ClientRefs,
  // which pin their streams; sends run on the process-global coropoll) and
  // never dereferences the EventRPCContext. It therefore needs no
  // EventRPCContext lifetime tracking; per-failed-tx traffic must not touch
  // context-owned synchronization after the snapshot is acquired.
  //
  // Per-indexer admission control (#3124) lives in relay_tx_failure: a report
  // is dropped for any indexer already at its in-flight cap, bounding retained
  // RPC state per indexer without letting one stuck indexer starve the others.
  indexer_snapshot_t *snapshot = try_acquire_indexer_snapshot();
  if (snapshot->get().clients.empty()) {
    // No indexers: release without spawning a coroutine frame at all.
    release_indexer_snapshot(snapshot);
    return;
  }

  [](indexer_snapshot_t *snapshot, SignedTransactionHash hash,
     TxFailureDiagnostic d) -> x23::Detached {
    x23::Defer release{[snapshot] { release_indexer_snapshot(snapshot); }};
    co_await relay_tx_failure(snapshot->get(), hash, d);
    co_return;
  }(snapshot, std::move(tx_hash), std::move(diag));
}

} // namespace hogd
