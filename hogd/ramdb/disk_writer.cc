#include "hogd/ramdb/disk_writer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/io.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

void
DiskWriter::write_worker(const std::stop_token &stoken)
{
  // initialize the buffer used for commit records.
  uint8_t commit_buf[sizeof(ItemHeader) + sizeof(uint32_t)];
  auto *commit_hdr =
      new (commit_buf) ItemHeader(ItemStorage::kCommit, 0, sizeof(uint32_t), 0);

  // called when a write to disk failed. wakes blocked commit() calls.
  auto signal_ioerror = [this] {
    std::scoped_lock guard(to_disk_lock_);
    io_error_ = true;
    commit_cv_.notify_all();
  };

  // used to calculate the CRC32C in the commit record.
  uint32_t crc = 0;

  while (true) {
    std::vector<UniqueLoglet> q;

    {
      // wait for work to become available.
      std::unique_lock ul(to_disk_lock_);
      worker_cv_.wait(ul, stoken, [this] { return !to_disk_.empty(); });
      if (to_disk_.empty() && stoken.stop_requested()) [[unlikely]]
        return;

      // take all the work that has been enqueued so far.
      std::swap(to_disk_, q);
    }

    // compute checksum and write to disk.
    for (UniqueLoglet &ll : q) {
      // Handle Commit Record
      if (!ll) {
        // prepare the commit record.
        commit_hdr->generation = synced_lsn_ + 1;
        crc = crc32c_append(crc, writable_byte_view(*commit_hdr));
        std::ranges::copy(writable_byte_view(crc),
                          commit_hdr->get_key().begin());
        crc = 0;

        // write the commit record and sync to the disk.
        ErrnoStatus<void> ret =
            write_full(w_, std::as_writable_bytes(std::span(commit_buf)));
        if (!ret)
          ret = w_.flush_and_sync();
        if (!ret) [[unlikely]] {
          signal_ioerror();
          return;
        }

        // wake the thread blocked in sync().
        {
          std::scoped_lock guard(to_disk_lock_);
          ++synced_lsn_;
          // only one thread will be waiting for this LSN, but efficiency
          // doesn't matter here so it's simpler to wake them all.
          commit_cv_.notify_all();
        }

        continue;
      }

      // Handle Change Records
      crc = crc32c_append(crc, ll->get_bytes());
      ErrnoStatus<void> ret = write_full(w_, ll->get_bytes());
      if (!ret) [[unlikely]] {
        signal_ioerror();
        return;
      }
      bytes_out_.store(bytes_out_.load(std::memory_order_relaxed) +
                           ll->get_bytes().size(),
                       std::memory_order_release);
      ll.reset(); // proactively free loglet
    }
  }
}

DBStatus<uint64_t>
DiskWriter::sync()
{
  std::unique_lock ul(to_disk_lock_);

  // Ask write_worker() to write the partial loglets in all heads and then the
  // commit record. write_worker() may also still be writing full loglets
  // requested from enqueue_to_disk().
  for (WriteHead &h : heads_) {
    if (!h.ll)
      continue;
    bytes_in_ += h.ll->get_bytes().size();
    to_disk_.emplace_back(std::move(h.ll));
  }
  to_disk_.emplace_back();
  worker_cv_.notify_one();

  // wait for the write_worker() to finish all pending writes before this LSN.
  uint64_t lsn = ++cur_lsn_;
  commit_cv_.wait(ul, [this, lsn] { return synced_lsn_ >= lsn || io_error_; });
  if (io_error_)
    return DBError(DBCode::kIO);
  return lsn;
}

void
DiskWriter::print_stats()
{
  size_t in, out;
  {
    std::scoped_lock guard(to_disk_lock_);
    in = bytes_in_;
  }
  out = bytes_out_.load(std::memory_order_relaxed);

  info("disk stats: bytes enqueued {}, bytes written {}", print_iec_bytes(in),
       print_iec_bytes(out));
}

void
DiskWriter::enqueue_to_disk(UniqueLoglet &&ll)
{
  std::scoped_lock guard(to_disk_lock_);
  bytes_in_ += ll->get_bytes().size();
  to_disk_.emplace_back(std::move(ll));
  worker_cv_.notify_one();
}

inline Loglet *
ScopedDiskWrite::get_loglet(size_t kv_len)
{
  assert(ul_.owns_lock());

  // Try to use the write head's current loglet.
  if (head_.ll && head_.ll->bytes_left() >= sizeof(ItemHeader) + kv_len)
      [[likely]] {
    return head_.ll.get();
  }

  // Otherwise the loglet is full or has not yet been allocated. Try to
  // allocate a new one, and enqueue the existing one to be written to disk.
  if (head_.ll)
    w_.enqueue_to_disk(std::move(head_.ll));
  head_.ll = make_unique_loglet(w_.allocator_, LogletMode::kDiskBuffer);
  return head_.ll.get();
}

DBStatus<void>
ScopedDiskWrite::modify(ItemHeader *src)
{
  assert(ul_.owns_lock());

  // get a loglet with enough space.
  Loglet *ll = get_loglet(src->kv_bytes());
  if (!ll) [[unlikely]]
    return DBError(DBCode::kNoMemory);

  // append the modification record.
  ItemHeader *dst = ll->clone(src);
  assert(dst != nullptr);
  std::ranges::copy(src->get_key_and_value(), dst->get_key_and_value().begin());
  return {};
}

DBStatus<void>
ScopedDiskWrite::remove(std::span<const std::byte> key)
{
  assert(ul_.owns_lock());

  // get a loglet with enough space.
  Loglet *ll = get_loglet(key.size());
  if (!ll) [[unlikely]]
    return DBError(DBCode::kNoMemory);

  // append the removal record.
  ItemHeader *hdr = ll->alloc(ItemStorage::kTombstone, 0, key.size(), 0);
  assert(hdr != nullptr);
  std::ranges::copy(key, hdr->get_key().begin());
  return {};
}

} // namespace hogd
