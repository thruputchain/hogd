#include "hogd/event/event_block_context.h"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <unistd.h>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/fastclock.h"

namespace hogd {

void
EventBlockContext::add_event(TxEventLog const &event)
{
  static PerThread<x23::Buf> serialize_buf;
  serialize_buf->resize_uninitialized(0);
  x23::serialize_to(&*serialize_buf, event);

  auto data = std::as_bytes(std::span(*serialize_buf));
  // Serialization of TxEventLog is key || value

  auto key = data.subspan(0, 32);
  auto value = data.subspan(32);

  write_event(std::span(*serialize_buf));
  CHECK(trie_.insert_or_update(key, value));
}

SHA256Hash
EventBlockContext::get_root_hash()
{
  return trie_.get_root_hash();
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
  buffers_->buf.append_range(data);
  buffers_->event_count++;
}

void
EventBlockContext::finish_file()
{
    auto ts = x23::fastclock::now();
  {
    auto ul = buffers_.lock_threads();
    uint32_t event_count = 0;
    buffers_.for_each(ul, [&event_count](EventContext &context) {
      debug("event count: {} buffer size: {}", context.event_count,
            context.buf.size());
      event_count += context.event_count;
    });

    if (auto r = x23::fullwrite(events_file_, x23::serialize(event_count)); !r)
        [[unlikely]] {
      panic("failed to write event count to file: {}", r.error());
    }
    auto ts2 = x23::fastclock::now();
    info("time to write event count: {} ns", ts2 - ts);

    size_t total_bytes = 0;

    buffers_.for_each(ul, [this, &total_bytes](EventContext &context) {
      total_bytes += context.buf.size();
      auto r = x23::fullwrite(events_file_, x23::BufView(context.buf));
      if (!r) [[unlikely]] {
        panic("failed to write events to file: {}", r.error());
      }
    });
    auto ts3 = x23::fastclock::now();
    info("time to write events: {} ns total bytes: {}", ts3 - ts2, total_bytes);
  }
  auto ts3 = x23::fastclock::now();

  std::unique_lock ul(file_mutex_);
  sync_file(ul);
  auto hash = get_root_hash();
  auto ts4 = x23::fastclock::now();
  rename_file(ul, hash);
  auto ts5 = x23::fastclock::now();
  info("time to sync {} ns, time to rename file: {} ns", ts4 - ts3, ts5 - ts4);
}

} // namespace hogd
