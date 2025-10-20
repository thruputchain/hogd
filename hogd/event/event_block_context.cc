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

namespace hogd {

void
EventBlockContext::add_event(TxEventLog const &event)
{
  static PerThread<x23::Buf> serialize_buf;
  serialize_buf->clear();
  x23::serialize_to(&*serialize_buf, event);

  auto data = std::as_bytes(std::span(*serialize_buf));
  // Serialization of TxEventLog is key || value

  auto key = data.subspan(0, 32);
  auto value = data.subspan(32);

  write_event(data);
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
EventBlockContext::write_event(std::span<const std::byte> data)
{
  buffers_->buf.append_range(x23::BufView(data));
  buffers_->event_count++;
}

void
EventBlockContext::finish_file()
{
  {
    auto ul = buffers_.lock_threads();
    uint32_t event_count = 0;
    buffers_.for_each(ul, [&event_count](EventContext &context) {
      event_count += context.event_count;
    });

    if (auto r = x23::fullwrite(events_file_, x23::serialize(event_count)); !r)
        [[unlikely]] {
      panic("failed to write event count to file: {}", r.error());
    }

    buffers_.for_each(ul, [this](EventContext &context) {
      auto r = x23::fullwrite(events_file_, x23::BufView(context.buf));
      if (!r) [[unlikely]] {
        panic("failed to write events to file: {}", r.error());
      }
    });
  }
  std::unique_lock ul(file_mutex_);
  sync_file(ul);
  auto hash = get_root_hash();
  rename_file(ul, hash);
}

} // namespace hogd
