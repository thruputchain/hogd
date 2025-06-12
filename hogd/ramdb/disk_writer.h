// disk_writer.h - the write path for persistent on-disk storage.
//
// Ramdb uses a multi-headed buffer design for writing to disk in order to
// improve multicore scalability. Each head is responsible for a shard of the
// key space and uses a loglet as its write buffer. When the loglet becomes
// full, it is enqueued to be written to the on-disk log. A seperate thread
// then concurrently performs the writes in the background.
//
// LSN = log sequence number

#pragma once

#include <cassert>
#include <mutex>
#include <thread>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

// forward declaration
class ScopedDiskWrite;

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

  DiskWriter(LogletAllocator &allocator, Directory &dir,
             std::string filename_format, size_t file_size_threshold,
             int write_head_order)
    : allocator_(allocator), filename_format_{std::move(filename_format)},
      dir_(dir), file_size_threshold_{file_size_threshold},
      heads_(1UL << write_head_order)
  {}
  ~DiskWriter() { writer_th_.request_stop(); }

  // Disable move and copy.
  DiskWriter(DiskWriter &&w) = delete;
  DiskWriter &operator=(DiskWriter &&w) = delete;
  DiskWriter(const DiskWriter &w) = delete;
  DiskWriter &operator=(const DiskWriter &w) = delete;

  // init must be called before using this class. It sets the initial LSN.
  void init(uint64_t lsn);

  // write_record returns a scoped handle that allows the caller to write to
  // the appropriate log head.
  //
  // Other writes to the same head will be locked, which allows the caller to
  // update the in-memory item before droping the scope so it can keep that
  // state consistent.
  ScopedDiskWrite write_record(size_t hash);

  // sync synchronizes all pending writes to disk. It returns a LSN, which can
  // be used to restore to this specific sync point after a failure.
  DBStatus<uint64_t> sync();

  // bytes_buffered returns the number of bytes currently buffered to be
  // written to disk.
  size_t bytes_buffered()
  {
    size_t bytes_in;
    {
      std::scoped_lock guard(to_disk_lock_);
      bytes_in = bytes_in_;
    }
    return bytes_in - bytes_out_.load(std::memory_order_acquire);
  }

  // print_stats prints disk usage statistics.
  void print_stats();

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
  const std::string filename_format_;
  // the directory containing the log files.
  Directory &dir_;
  // log file size before rolling over to a new file/lsn.
  size_t file_size_threshold_;

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
  // value of bytes_out_ last time it was printed. used for running bw stats.
  size_t prev_bytes_out_{0};
  // timestamp of last time disk stats was printed. used for running bw stats.
  using time_point =
      std::chrono::time_point<std::chrono::high_resolution_clock>;
  time_point prev_stats_time_;
  // loglets waiting to be written to disk. An empty UniqueLoglet triggers a
  // commit and advances the LSN.
  std::vector<UniqueLoglet> to_disk_;
  // wakes write_worker().
  std::condition_variable_any worker_cv_;
  // used to wake commit().
  std::condition_variable commit_cv_;

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
  {}
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
