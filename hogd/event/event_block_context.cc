#include "hogd/event/event_block_context.h"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unistd.h>
#include <utility>
#include <vector>
#include <version>

#include "event.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

constexpr static size_t kBufferThreshold = 1_MiB;

namespace {
// TODO(geoff): Is there any benefit to recycling the MerkleTriePrefix struct?
x23::Detached
async_free_trie(std::unique_ptr<MerkleTriePrefix> trie)
{
  co_await default_threadpool().bg();
  trie.reset();
  co_return;
}
} // namespace

void
EventBlockContext::add_event(TxEventLog const &event)
{
  static PerThread<x23::Buf> serialize_buf;
  serialize_buf->resize_uninitialized(0);
  x23::serialize_to(&*serialize_buf, event);
  assert(serialize_buf->size() >= 32);

  auto data = std::as_bytes(std::span(*serialize_buf));
  // Serialization of TxEventLog is key || value

  assert(data.size() >= 32);

  auto key = data.subspan(0, 32);
  auto value = data.subspan(32);

  write_event(std::span(*serialize_buf));
  CHECK(trie_->insert_or_update(key, value));
}

SHA256Hash
EventBlockContext::get_root_hash()
{
  trie_->generate_top_level_hashes();
  return trie_->get_root_hash();
}

void
EventBlockContext::sync_file(std::unique_lock<std::mutex> &ul)
{
  assert(ul.owns_lock());
  assert(ul.mutex() == &file_mutex_);

  assert(events_file_);
  if (fsync(events_file_.get()) != 0) {
    panic("failed to fsync events file: {}", strerror(errno));
  }
}

void
EventBlockContext::rename_file(std::unique_lock<std::mutex> &ul,
                               SHA256Hash const &hash)
{
  assert(ul.owns_lock());
  assert(ul.mutex() == &file_mutex_);

  auto old_name = temp_events_filename(work_directory_, block_id_);
  auto new_name = final_events_filename(work_directory_, hash);
  int rc = std::rename(old_name.c_str(), new_name.c_str());
  if (rc != 0) {
    panic("failed to rename events file '{}' to '{}': {}", old_name, new_name,
          strerror(errno));
  }
}

void
EventBlockContext::write_event(std::span<const uint8_t> data)
{
#if __cpp_lib_containers_ranges >= 202202L
  buffers_->buf.append_range(data);
#else
  buffers_->buf.insert(buffers_->buf.end(), data.begin(), data.end());
#endif
  buffers_->event_count++;

  if (buffers_->buf.size() >= kBufferThreshold) {
    pending_writes_++;
    [](std::vector<uint8_t> buf, EventBlockContext *self) -> x23::Detached {
      co_await default_threadpool().bg();
      {
        std::lock_guard lock(self->file_mutex_);
        auto ts = x23::fastclock::now();
        auto r = x23::fullwrite(self->events_file_, x23::BufView(buf));
        if (!r) [[unlikely]] {
            panic("failed to write events to file: {}", r.error());
        }
        if (self->pending_writes_.fetch_sub(1) == 1) {
            self->file_cv_.notify_one();
        }
      
        auto ts2 = x23::fastclock::now();
        info("time to write events: {} ns bytes={}", ts2 - ts, buf.size());
      }
      co_return;
    }(std::move(buffers_->buf), this);
    buffers_->buf.clear();
  }
}

void
EventBlockContext::finish_file_write()
{
  auto ts = x23::fastclock::now();

  std::unique_lock<std::mutex> file_ul;
  size_t total_bytes = 0;

  {
    file_ul = std::unique_lock<std::mutex>(file_mutex_);
    file_cv_.wait(file_ul, [this] { return pending_writes_ == 0; });

    auto ul = buffers_.lock_threads();

    buffers_.for_each(ul, [this, &total_bytes](EventContext &context) {
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
  assert(file_ul.owns_lock());

  sync_file(file_ul);
  auto ts4 = x23::fastclock::now();
  info("time to sync {} ns,", ts4 - ts3);
}

void
EventBlockContext::finish_file(SHA256Hash const &hash)
{
  std::unique_lock<std::mutex> file_ul(file_mutex_);
  auto ts4 = x23::fastclock::now();
  rename_file(file_ul, hash);
  auto ts5 = x23::fastclock::now();
  info("time to rename file: {} ns", ts5 - ts4);
}

EventBlockContext::~EventBlockContext()
{
  std::unique_lock ul(file_mutex_);
  file_cv_.wait(ul, [this] { return pending_writes_ == 0; });
  async_free_trie(std::move(trie_));
}

} // namespace hogd
