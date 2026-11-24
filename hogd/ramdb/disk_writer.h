// disk_writer.h - the write path for persistent on-disk storage.
//
// Ramdb uses a multi-headed buffer design for writing to disk in order to
// improve multicore scalability. Each head is responsible for a shard of the
// key space and uses a loglet as its write buffer. When the loglet becomes
// full, it is enqueued to be written to the on-disk log. A separate thread
// then concurrently performs the writes in the background.
//
// LSN = log sequence number

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/metrics/metrics.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

// forward declaration
class ScopedDiskWrite;

enum class SyncStatsMode { kDisabled, kEnabled };

// SyncStats describes one completed sync(). The caller and writer thread
// measure wall time independently, so some phases overlap.
struct SyncStats {
  using ns = std::chrono::nanoseconds;
  // the LSN this sync() returned.
  uint64_t lsn{0};
  // heads that held a partial loglet when sync() drained them.
  size_t dirty_heads{0};
  // bytes drained from the heads by sync().
  size_t bytes_drained{0};
  // bytes of loglets the writer started after the previous sync and before
  // this sync started draining the heads, even if they finished later.
  size_t bytes_early{0};
  // bytes of loglets the writer started at or after the start of the drain:
  // bytes_drained plus full loglets that were still queued.
  size_t bytes_synced{0};
  // loglets in bytes_synced.
  size_t loglets_synced{0};
  // sync(): from entry through locking the queue, reserving space, and
  // queueing the partial loglets. This can overlap work on the writer thread.
  ns drain{0};
  // writer: remaining time for a loglet in progress at the start of the drain.
  ns inflight{0};
  // writer: CRC32C of the loglets in bytes_synced.
  ns crc{0};
  // writer: write_full() of the loglets in bytes_synced through the
  // BufferedWriter: copies into its buffer and the write() calls.
  ns write{0};
  // writer: write the commit record and flush the buffer.
  ns flush{0};
  // writer: fdatasync().
  ns fdatasync{0};
  // sync(): wall time from call to return, including lock handoff and wakeup.
  ns total{0};
};

// DiskWriter is the main class for writing records to the on-disk log.
//
// The typical usage steps are as follows:
//
// 1. Call write_record() for each item change to get the right log head.
// 2. Use the ScopedDiskWrite it returns to make the actual change on the
// appropriate log head (either by calling modify() or remove()).
// 3. Modify the in-memory items to reflect the change before dropping the scope
// (for synchronization to ensure memory is modified in the same order).
//
// Repeat 1-3 until done.
//
// Then, when all the changes are finished, call sync() to ensure the on-disk
// log is durable. It will not return until everything is safely flushed to
// disk.
class DiskWriter {
public:
  friend ScopedDiskWrite;

  DiskWriter(LogletAllocator &allocator, Directory &dir, int write_head_order,
             std::function<void()> wal_grew_observer = nullptr)
    : allocator_(allocator), dir_(dir),
      wal_grew_observer_{std::move(wal_grew_observer)},
      heads_(1UL << write_head_order)
  {}
  ~DiskWriter() { writer_th_.request_stop(); }

  // Disable move and copy.
  DiskWriter(DiskWriter &&w) = delete;
  DiskWriter &operator=(DiskWriter &&w) = delete;
  DiskWriter(const DiskWriter &w) = delete;
  DiskWriter &operator=(const DiskWriter &w) = delete;

  // init must be called before using this class. It sets the initial LSN,
  // what filenames the files is produces should be called, and
  // how long each log file should be before rolling over to a new log file.
  void init(uint64_t lsn, std::string_view filename_format,
            size_t log_file_size_threshold,
            SyncStatsMode sync_stats = SyncStatsMode::kDisabled);

  // write_record returns a scoped handle that allows the caller to write to
  // the appropriate log head.
  //
  // Other writes to the same head will be locked, which allows the caller to
  // update the in-memory item before dropping the scope so it can keep that
  // state consistent.
  ScopedDiskWrite write_record(size_t hash);

  // sync synchronizes all pending writes to disk. It returns a LSN, which can
  // be used to restore to this specific sync point after a failure. Only one
  // sync() may run at a time.
  DBStatus<uint64_t> sync();

  // bytes_buffered returns the number of bytes currently buffered to be
  // written to disk.
  size_t bytes_buffered()
  {
    // Read both under the lock: bytes enter bytes_in_ before the writer can
    // dequeue them, so the written total cannot pass the queued total here.
    std::scoped_lock guard(to_disk_lock_);
    return bytes_in_ - bytes_out_.load(std::memory_order_acquire);
  }

  // Returns the last successful sync's stats if collection was enabled.
  std::optional<SyncStats> last_sync_stats()
  {
    std::scoped_lock guard(to_disk_lock_);
    return last_sync_stats_;
  }

  // print_stats prints disk usage statistics.
  void print_stats();

  void register_metrics();
  void record_metrics();

  size_t get_bytes_out() const
  {
    return bytes_out_.load(std::memory_order_relaxed);
  }

  // size in bytes a log file must be before writer creates a new file.
  size_t get_file_size_threshold() const { return file_size_threshold_; }

private:
  struct WriteHead {
    std::mutex lock;
    UniqueLoglet ll;
  };

  // the allocator for loglets.
  LogletAllocator &allocator_;
  // format string for how filenames should be generated.
  // should contain exactly one {} which indicates where lsn or
  // chunk number should be inserted into the generated filenames.
  std::string filename_format_;
  // the directory containing the log files.
  Directory &dir_;
  // log file size before rolling over to a new file/lsn.
  size_t file_size_threshold_;
  // called when writer creates a new log file.
  std::function<void()> wal_grew_observer_;
  // Set by init() before starting the writer thread.
  bool collect_sync_stats_{false};

  // protects everything in this block.
  std::mutex to_disk_lock_;
  // set to true if DBCode::kIO occurs in write_worker().
  bool io_error_{false};
  // the highest LSN requested to be written to disk.
  uint64_t cur_lsn_;
  // the last LSN fully written to disk.
  uint64_t synced_lsn_;
  // the bytes requested to be written to disk (not including commit records).
  size_t bytes_in_{0};
  // the bytes actually written to disk (not including commit records).
  std::atomic<size_t> bytes_out_{0};
  Metrics::Gauge bytes_out_metric_;
  // value of bytes_out_ last time it was printed. used for running bw stats.
  size_t prev_bytes_out_{0};
  // timestamp of last time disk stats was printed. used for running bw stats.
  using time_point = std::chrono::time_point<std::chrono::steady_clock>;
  time_point prev_stats_time_;
  // loglets waiting to be written to disk. An empty UniqueLoglet triggers a
  // commit and advances the LSN.
  std::vector<UniqueLoglet> to_disk_;
  // wakes write_worker().
  std::condition_variable_any worker_cv_;
  // used to wake commit().
  std::condition_variable commit_cv_;
  // stats of the last completed sync().
  std::optional<SyncStats> last_sync_stats_;
  // write_worker()'s part of the stats of the sync it just committed. sync()
  // adds its own fields and publishes the result as last_sync_stats_.
  SyncStats writer_stats_;

  // steady_clock time just before this sync() drains the heads, or 0 when
  // inactive. The writer uses it to attribute work to the sync.
  std::atomic<std::chrono::steady_clock::rep> sync_begin_{0};

  // True while sync() runs; ScopedDiskWrite CHECKs it is clear after
  // acquiring its head lock, in release builds too, since an overlapping
  // write corrupts the WAL undetectably (#3397). Together with the try_lock
  // CHECK in sync()'s head drain this covers every overlap: a writer that
  // locks its head first fails the drain's try_lock, and one that locks
  // after the drain sees the flag through the head mutex's happens-before,
  // so relaxed loads suffice.
  std::atomic<bool> sync_active_{false};

  // loglets that are currently being filled with changes to log to disk
  // (called "heads"). Items are sharded across the vector based on their
  // hash. When a head is full, it is added to the to_disk_ queue.
  //
  // heads_.size() must be a power of 2.
  //
  // Cache aligned to avoid false sharing with the disk writing state above.
  alignas(kCacheLineSize) std::vector<WriteHead> heads_;

  // runs write_worker().
  std::jthread writer_th_;

  // get_head returns the loglet head to use for this hash's shard.
  WriteHead &get_head(size_t hash)
  {
    assert(std::has_single_bit(heads_.size()));
    return heads_[hash & (heads_.size() - 1)];
  }

  // create_next_file returns the next file to use for writing the log or
  // snapshot.
  DBStatus<File> create_next_file();
  // a run loop that stores items from to_disk_ to the on-disk log.
  void write_worker(const std::stop_token &stoken);
  // enqueue_to_disk enqueues a loglet to be written to disk.
  void enqueue_to_disk(UniqueLoglet &&ll);
};

// ScopedDiskWrite provides a handle to append changes to the on-disk log for a
// given key's hash. The handle is tied to a specific log head. It blocks
// (while in scope) further modifications to that head, which is necessary for
// synchronizing updates with in-memory items.
//
// To get good performance, hold the scope for as short a time as possible
// (like a lock).
class ScopedDiskWrite {
public:
  ScopedDiskWrite(DiskWriter &w, size_t hash)
    : w_(w), head_(w.get_head(hash)), ul_(head_.lock)
  {
    // Checked under the head lock: if sync() already drained this head, its
    // unlock happens-before this acquisition, so the load must see true. A
    // check before the lock would leave a gap — a writer paused between
    // checking and locking evades both this and the drain's try_lock CHECK.
    // No write may overlap sync(); see sync_active_.
    CHECK(!w_.sync_active_.load(std::memory_order_relaxed));
  }
  ~ScopedDiskWrite() = default;

  // Disable move and copy.
  ScopedDiskWrite(ScopedDiskWrite &&w) = delete;
  ScopedDiskWrite &operator=(ScopedDiskWrite &&w) = delete;
  ScopedDiskWrite(const ScopedDiskWrite &w) = delete;
  ScopedDiskWrite &operator=(const ScopedDiskWrite &w) = delete;

  // modify appends a change to an item in the log. The change is copied from
  // src. It could either be creating a new item or modifying an existing item.
  // On success returns the lsn of the item created in the log.
  DBStatus<uint64_t> modify(ItemHeader *src, bool use_src_lsn = false);
  // remove appends an item's deletion to the log.
  DBStatus<void> remove(std::span<const std::byte> key);

  // reset drops the lock on the log head. add() and remove() cannot be used
  // after calling this.
  void reset() { ul_.unlock(); }

private:
  using Head = DiskWriter::WriteHead;

  // the DiskWriter that contains the head for this scope.
  DiskWriter &w_;
  // the head to append changes to.
  Head &head_;
  // the lock protecting the head (must be held to append changes).
  std::unique_lock<std::mutex> ul_;

  // get_loglet returns a log head for writing, or nullptr if out of memory.
  Loglet *get_loglet(size_t kv_len);
};

inline ScopedDiskWrite
DiskWriter::write_record(size_t hash)
{
  return ScopedDiskWrite(*this, hash);
}

} // namespace hogd
