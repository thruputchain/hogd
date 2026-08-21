#include "hogd/hog_event/event_block_context.h"
#include "event_outbox_internal.hh"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <sys/fcntl.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <version>

#include "event.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/debug.h"
#include "x23/fastclock.h"
#include "x23/fileio.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

constexpr static size_t kBufferThreshold = 500_KiB;

void
EventBlockContext::open_events_file()
{
  auto dir = Directory::open(work_directory_);
  if (!dir) {
    panic("failed to open work directory: {}", dir.error());
  }
  auto filename = temp_events_filename(work_directory_, block_id_);
  // NOLINTNEXTLINE(misc-include-cleaner)
  auto fd = open(filename.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd == -1) {
    panic("failed to open events file: {}", strerror(errno));
  }
  events_file_ = x23::UniqueFd(fd);
}

EventBlockContext::EventBlockContext(std::string_view work_directory,
                                     BlockID block_id,
                                     ReusableEventBlockContextData data)
  : trie_(std::move(data.trie)), work_directory_(work_directory),
    block_id_(block_id),
    events_with_bodies_(std::move(data.events_with_bodies)),
    buffers_(std::move(data.buffers))
{
  trie_->clear();
  {
    auto ul = buffers_.lock_threads();
    buffers_.for_each(ul, [](detail::EventContext &ctx) {
      ctx.buf.clear();
      ctx.event_count = 0;
    });
  }
  {
    auto ul = events_with_bodies_.lock_threads();
    events_with_bodies_.for_each(ul, [](EventWithBodyBatch &batch) {
      batch.clear();
    });
  }
  open_events_file();
}

ReusableEventBlockContextData
EventBlockContext::extract_data()
{
  return ReusableEventBlockContextData(std::move(trie_),
                                       std::move(events_with_bodies_),
                                       std::move(buffers_));
}

x23::Async<bool>
EventBlockContext::add_event(TxEventLog event, ProposedTransaction body)
{
  if (open_executor_handles_.load(std::memory_order_acquire) == 0) {
    co_return false;
  }

  static PerThread<x23::Buf> serialize_buf;
  const size_t event_xdr_size =
      serialize_event_commitment_leaf(*serialize_buf, event,
                                      body.nondeterministic_proposal_data);

  auto data = std::as_bytes(std::span(*serialize_buf));
  // The commitment leaf is key || value.
  assert(data.size() >= 32);

  auto key = data.subspan(0, 32);
  auto value = data.subspan(32);

  // WARNING: insert must happen BEFORE write_event.  write_event may
  // co_await internally, which can resume the coroutine on a different thread.
  // key and value are spans into the static PerThread serialize_buf for this
  // thread — if we co_await first, another coroutine on the original thread
  // could overwrite serialize_buf, leaving key/value pointing at stale or freed
  // memory.
  CHECK(trie_->insert(key, value));
  using write_event_result_t =
      decltype(std::declval<EventBlockContext &>().write_event(
          std::declval<std::span<const uint8_t>>()));
  static_assert(
      std::same_as<decltype(std::declval<write_event_result_t::promise_type &>()
                                .initial_suspend()),
                   std::suspend_never>);
  co_await write_event(std::span(*serialize_buf).first(event_xdr_size));

  // Preserve each event/body pair when the durable candidate is snapshotted at
  // close. The event trie commits to the body's nondeterministic data but not
  // its signature wrapper.
  events_with_bodies_->emplace_back(
      detail::EventWithBody{.event = std::move(event),
                            .body = std::move(body)});
  co_return true;
}

SHA256Hash
EventBlockContext::get_root_hash()
{
  trie_->generate_all_hashes(available_parallelism());
  return trie_->get_root_hash();
}

void
EventBlockContext::sync_file(x23::CoroGuard const & /*unused*/)
{
  assert(events_file_);
  if (fsync(events_file_.get()) != 0) {
    panic("failed to fsync events file: {}", strerror(errno));
  }
}

void
EventBlockContext::rename_file(x23::CoroGuard const & /*unused*/,
                               SHA256Hash const &hash)
{
  auto old_name = temp_events_filename(work_directory_, block_id_);
  auto new_name = final_events_filename(work_directory_, hash);
  int rc = std::rename(old_name.c_str(), new_name.c_str());
  if (rc != 0) {
    panic("failed to rename events file '{}' to '{}': {}", old_name, new_name,
          strerror(errno));
  }
}

x23::Detached
EventBlockContext::flush_buffer_async(std::vector<uint8_t> buf)
{
  co_await get_threadpool().bg();

  {
    x23::CoroGuard g;
    co_await g(lock_);

    if (state_ == BlockState::kAbandoned) {
      // nothing to do, just decrement the pending writes count
      if (--pending_write_count_ == 0) {
        all_writes_done_.store(true);
        all_writes_done_.broadcast();
      }
      co_return;
    }

    if (state_ == BlockState::kFinished) {
      panic("attempted to flush buffer after block has begun shutting down");
    }
    x23assert(state_ == BlockState::kActive ||
              state_ == BlockState::kFinishing);

    auto r = x23::fullwrite(events_file_, x23::BufView(buf));
    if (!r) [[unlikely]] {
      panic("failed to write events to file: {}", r.error());
    }
    if (--pending_write_count_ == 0 && state_ == BlockState::kFinishing) {
      all_writes_done_.store(true);
      all_writes_done_.broadcast();
    }
  }
  // buf clear is out of the critical section holding the lock
  buf.clear();
  co_return;
}

x23::Async<void>
EventBlockContext::write_event(std::span<const uint8_t> data)
{
  // DO NOT SUSPEND while still using the data parameter!  That would hold a
  // reference to PerThread data across a coroutine suspend point!
#if __cpp_lib_containers_ranges >= 202202L
  buffers_->buf.append_range(data);
#else
  buffers_->buf.insert(buffers_->buf.end(), data.begin(), data.end());
#endif
  buffers_->event_count++;

  // data is no longer used; clear the reference to the span just to be
  // defensive.
  data = {};

  if (buffers_->buf.size() >= kBufferThreshold) {
    // Extract the buffer from the current thread's PerThread slot BEFORE the
    // co_await.  After co_await, the coroutine may resume on a different
    // thread, so buffers_-> would resolve to that thread's slot instead of the
    // one that exceeded the threshold.
    using buf_t = std::decay_t<decltype(buffers_->buf)>;
    buf_t buf = std::move(buffers_->buf);
    buffers_->buf.clear();
    {
      // By the time this coroutine returns, pending_write_count_ will have been
      // incremented or the state_ has already been set as kAbandoned or
      // kFinishing and we do not spawn a new detached coroutine
      x23::CoroGuard g;
      co_await g(lock_);
      if (state_ != BlockState::kActive) {
        x23assert(state_ == BlockState::kAbandoned ||
                  state_ == BlockState::kFinishing);
        if (state_ == BlockState::kFinishing) {
          // Insert into the current thread's PerThread slot (which may differ
          // from the slot we extracted from before co_await).
          // finish_file_write() drains every slot via for_each(), so the data
          // will be captured regardless of which slot holds it.  We hold lock_,
          // so finish_file_write() cannot iterate until we release.
          buffers_->buf.insert(buffers_->buf.end(), buf.begin(), buf.end());
        }
        co_return;
      }
      pending_write_count_++;
    }

    flush_buffer_async(std::move(buf));
  }
}

x23::Async<void>
EventBlockContext::await_pending_writes_done()
{
  [[maybe_unused]] bool looped = false;
  while (true) {
    {
      x23::CoroGuard g;
      co_await g(lock_);
      x23assert(looped || state_ == BlockState::kActive ||
                state_ == BlockState::kAbandoned);
      if (state_ == BlockState::kActive) {
        state_ = BlockState::kFinishing;
      }
      looped = true;
      if (pending_write_count_ == 0) {
        co_return;
      }
    }
    co_await all_writes_done_.wait(false);
  }
}

x23::Async<void>
EventBlockContext::finish_file_write()
{
  x23assert(open_executor_handles_.load(std::memory_order_acquire) == 0);

  auto ts = x23::fastclock::now();
  size_t total_bytes = 0;

  co_await await_pending_writes_done();

  x23::CoroGuard g;
  co_await g(lock_);
  x23assert(state_ == BlockState::kFinishing);
  state_ = BlockState::kFinished;

  {
    auto ul = buffers_.lock_threads();

    buffers_.for_each(ul, [this, &total_bytes](detail::EventContext &context) {
      total_bytes += context.buf.size();
      auto r = x23::fullwrite(events_file_, x23::BufView(context.buf));
      if (!r) [[unlikely]] {
        panic("failed to write events to file: {}", r.error());
      }
    });
  }

  auto ts3 = x23::fastclock::now();
  info("time to write events: {} ns total bytes at end: {}", ts3 - ts,
       total_bytes);

  sync_file(g);
  auto ts4 = x23::fastclock::now();
  info("time to sync {} ns,", ts4 - ts3);
}

x23::Async<void>
EventBlockContext::finish_file(SHA256Hash const &hash)
{
  x23::CoroGuard g;
  co_await g(lock_);
  root_hash_ = hash;
  has_root_hash_ = true;
  auto ts4 = x23::fastclock::now();
  rename_file(g, hash);
  auto ts5 = x23::fastclock::now();
  info("time to rename file: {} ns", ts5 - ts4);
  auto directory = Directory::open(work_directory_, /*create=*/false);
  if (!directory) {
    panic("failed to open events directory after rename: {}",
          directory.error());
  }
  if (auto synced = directory->sync(); !synced) {
    panic("failed to fsync events directory after rename: {}", synced.error());
  }
  file_done_.store(true);
  auto *pool = x23::ThreadPool::get_thread_pool();
  if (!pool) {
    pool = &default_threadpool();
  }
  file_done_.broadcast(pool);
}

x23::Async<IndexerOutboxCandidatePayload>
EventBlockContext::snapshot_indexer_outbox_candidate(BlockHeight block_height,
                                                     SHA256Hash event_root)
{
  IndexerOutboxCandidatePayload payload{
      .format_version = kIndexerOutboxFormatVersion,
      .block_id = block_id_,
      .block_height = block_height,
      .event_root = event_root,
  };

  // Each EventWithBody was created as one pair by add_event. Flatten the
  // per-thread batches in lockstep; the indexer is insensitive to pair order.
  std::unique_lock ul = events_with_bodies_.lock_threads();
  size_t pair_count = 0;
  events_with_bodies_.for_each(ul,
                               [&pair_count](const EventWithBodyBatch &batch) {
                                 pair_count += batch.size();
                               });
  payload.event_logs.reserve(pair_count);
  payload.tx_bodies.reserve(pair_count);
  events_with_bodies_.for_each(ul, [&payload](EventWithBodyBatch &batch) {
    for (detail::EventWithBody &pair : batch) {
      payload.event_logs.push_back(std::move(pair.event));
      payload.tx_bodies.push_back(std::move(pair.body));
    }
  });
  co_return payload;
}

x23::Async<void>
EventBlockContext::wait_file_done()
{
  while (!file_done_.load()) {
    co_await file_done_.wait(false);
  }
}

void
EventBlockContext::delete_events_file()
{
  events_file_.reset();
  if (has_root_hash_) {
    auto name = final_events_filename(work_directory_, root_hash_);
    if (std::remove(name.c_str()) != 0) {
      warn("delete_events_file: failed to remove final file '{}': {}", name,
           strerror(errno));
    }
  }
  else {
    auto name = temp_events_filename(work_directory_, block_id_);
    if (std::remove(name.c_str()) != 0) {
      warn("delete_events_file: failed to remove temp file '{}': {}", name,
           strerror(errno));
    }
  }
}

x23::Async<void>
EventBlockContext::await_abandon()
{
  {
    x23::CoroGuard g;
    co_await g(lock_);
    state_ = BlockState::kAbandoned;
    all_executors_closed_.store(true);
    auto *pool = x23::ThreadPool::get_thread_pool();
    if (!pool) {
      pool = &default_threadpool();
    }
    all_executors_closed_.broadcast(pool);
  }
  co_await await_pending_writes_done();
  co_return;
}

EventBlockContext::~EventBlockContext()
{
  assert(state_ != BlockState::kActive);
  assert(pending_write_count_ == 0);

  trie_.reset();
}

void
EventBlockContext::executor_open_block()
{
  open_executor_handles_.fetch_add(1, std::memory_order_release);
  x23assert(all_executors_closed_.load() == false);
}

void
EventBlockContext::executor_close_block()
{
  uint32_t prev =
      open_executor_handles_.fetch_sub(1, std::memory_order_release);
  // Block ids are never reused, so an unmatched or duplicate close cannot
  // arise from RPC timing -- it is a bug in the (trusted) executor. CHECK
  // rather than x23assert: under NDEBUG the unsigned underflow would
  // silently wedge await_all_executors_closed() -- and with it every
  // subsequent close of this block -- forever.
  CHECK(prev > 0);
  if (prev == 1) {
    all_executors_closed_.store(true);
    auto *pool = x23::ThreadPool::get_thread_pool();
    if (!pool) {
      pool = &default_threadpool();
    }
    all_executors_closed_.broadcast(pool);
  }
}

x23::Async<bool>
EventBlockContext::await_all_executors_closed()
{
  while (open_executor_handles_.load(std::memory_order_acquire) > 0) {
    co_await all_executors_closed_.wait(false);
    x23::CoroGuard g;
    co_await g(lock_);
    if (state_ == BlockState::kAbandoned) {
      co_return false;
    }
  }
  co_return true; // All executors closed normally
}

} // namespace hogd
