// db.h - the main interface to the database

#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#include "hogd/base/io.h"
#include "hogd/base/os.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/metrics/metrics.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/item_allocator.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

// GetEachCallback is used by get_each(). Arguments are key, type, and value.
using GetEachCallback = std::function<void(std::span<const std::byte>, uint8_t,
                                           std::span<const std::byte>)>;

// LayoutCallback is used by for_each_location(). Arguments are the item's key,
// its total size in bytes (header included), and the index of the loglet
// holding it.
using LayoutCallback =
    std::function<void(std::span<const std::byte>, size_t, size_t)>;

// Database provides the interface for using ramdb. Normally, you should only
// interact with this class.
class Database {
public:
  //
  // Factory methods: Use these to initialize a database.
  //

  // open opens a database at path.
  static DBStatus<std::unique_ptr<Database>>
  open(std::string_view path, size_t heap_size, int bucket_order = 0,
       size_t log_file_size_threshold = kDiskLogFileThreshold);

  // open_lsn opens a database at path and loads only up to a specific LSN.
  static DBStatus<std::unique_ptr<Database>>
  open_lsn(std::string_view path, uint64_t lsn, size_t heap_size,
           int bucket_order = 0,
           size_t log_file_size_threshold = kDiskLogFileThreshold);

  // check checks the integrity of a database. It returns the highest valid LSN.
  static DBStatus<uint64_t> check(std::string_view path, size_t heap_size);

  Database(Directory dir, size_t heap_size, int lock_order, int bucket_order,
           int write_head_order)
    : dir_(std::move(dir)),
      loglet_allocator_(LogletAllocator::create(
          heap_size, available_parallelism() * kCompactionLogletsPerThread,
          [this] { notify_low_mem(); })),
      pool_(available_parallelism()),
      item_allocator_(*loglet_allocator_, pool_, &map_),
      disk_writer_(*loglet_allocator_, dir_, write_head_order,
                   [this] { notify_wal_grew(); }),
      map_(item_allocator_, disk_writer_, lock_order, bucket_order),
      bg_th_(
          [this](const std::stop_token &stoken) { background_worker(stoken); })
  {}
  ~Database() = default;

  // Disable move and copy.
  Database(Database &&db) = delete;
  Database &operator=(Database &&db) = delete;
  Database(const Database &db) = delete;
  Database &operator=(const Database &db) = delete;

  // get finds an item for a given key.
  //
  // Read-only: safe to call concurrently with put()/remove() and with sync().
  DBStatus<DBItem> get(std::span<const std::byte> key);

  // get_if_type finds an item of a specific type for a given key. If the type
  // does not match, it will return DBCode::kTypeMismatch.
  DBStatus<OwningSpan<const std::byte>>
  get_if_type(std::span<const std::byte> key, uint8_t type);

  // get_if_type finds an item of a specific type for a given key. If the
  // type or the size of val_out do not match exactly, it will return
  // DBCode::kTypeMismatch.
  DBStatus<void> get_if_type(std::span<const std::byte> key, uint8_t type,
                             std::span<std::byte> val_out);

  // get_size returns the size of the item (if it exists).
  DBStatus<size_t> get_size(std::span<const std::byte> key);

  // get_type returns the type of the item (if it exists).
  DBStatus<uint8_t> get_type(std::span<const std::byte> key);

  // item_fits reports whether an item with the given key and value lengths is
  // within the storage limits put() enforces (otherwise put() returns
  // DBCode::kTooBig). Lets callers pre-validate a write before it lands.
  static constexpr bool item_fits(size_t key_len, size_t value_len)
  {
    return ScalableMap::item_fits(key_len, value_len);
  }

  // put inserts an item.
  //
  // Concurrent put()/remove() calls are safe, but a put() must never overlap a
  // sync(): doing so is a data race / undefined behavior. See sync().
  DBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                     std::span<const std::byte> val);

  // remove deletes an item. It is not considered an error if the item does not
  // exist.
  //
  // Same concurrency rules as put(): concurrent put()/remove() are safe, but a
  // remove() must never overlap a sync() (data race / undefined behavior). See
  // sync().
  DBStatus<void> remove(std::span<const std::byte> key);

  // get_each reads every item and provides its key, type, and value to a
  // callback.
  void get_each(GetEachCallback fn);

  // get_each_parallel reads every item and provides its key, type, and value to
  // a callback that runs in parallel on a worker pool.
  void get_each_parallel(GetEachCallback fn,
                         size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool());

  // for_each_location reports which loglet holds each live item. Joining the
  // loglet index against an externally recorded access set yields per-loglet
  // hot-byte utilization, which get_each() cannot produce because it does not
  // expose item locations.
  //
  // Read-only, with the same rules as get_each(), but the layout it reports is
  // only meaningful if no compaction is relocating items underneath it.
  void for_each_location(LayoutCallback fn);

  // loglet_count returns the number of loglets the heap region is divided
  // into. Each is one tiering unit of kLogletRegionSize bytes.
  size_t loglet_count() const { return loglet_allocator_->loglet_count(); }

  // sync writes pending items to disk. Any calls to put() or remove() performed
  // before this call are guaranteed to be recoverable after a failure.
  // returns an lsn that can be passed to snapshot() to indicate which
  // items should be included in a snapshot.
  //
  // PRECONDITION: no put() or remove() may overlap a sync(). The caller must
  // ensure every put()/remove() has returned and happens-before this call. An
  // overlapping write is a data race / undefined behavior: sync() moves the
  // per-head write buffers that put()/remove() mutate. Read-only access (get(),
  // get_if_type(), get_each(), ...) is safe to run concurrently with sync().
  DBStatus<uint64_t> sync();

  // write all of the state changes that happened after begin_lsn.
  // begin_lsn must be 0 (full snapshot) or an end_lsn returned by a prior
  // call to sync_and_snapshot() on this Database instance (or loaded from
  // disk during recovery). Using an LSN from sync() that exceeds the
  // snapshot coverage will be rejected to prevent unrecoverable gaps.
  DBStatus<std::pair<uint64_t, std::future<DBStatus<void>>>>
  sync_and_snapshot(uint64_t begin_lsn);

  // set_active indicates to the database that it is in a state of activity (if
  // yes is true) or inactivity (if yes is false).  When inactive, items should
  // not be accessed (e.g., get(), put(), etc.); this allows the database to
  // run background activities that could disrupt performance and may not be
  // thread safe.
  void set_active(bool yes);

  void register_metrics();
  void record_metrics();

  // disk_bytes_written returns the total bytes written to WAL log files since
  // the last database recovery.
  size_t disk_bytes_written() const { return disk_writer_.get_bytes_out(); }

  // snapshot_bytes_written returns the total bytes written to snapshot files
  // since the last database recovery.
  size_t snapshot_bytes_written() const
  {
    return metrics_.snap_bytes_out_.load(std::memory_order_relaxed);
  }

  // compaction_passes returns how many times the compactor has run since the
  // last recovery, and compaction_bytes_reclaimed the memory it gave back.
  // Compaction is demand-driven, so a workload that never pressures memory
  // leaves both at zero -- which is itself worth asserting in experiments that
  // mean to exercise the compactor.
  uint64_t compaction_passes() const
  {
    return compaction_passes_.load(std::memory_order_relaxed);
  }
  uint64_t compaction_bytes_reclaimed() const
  {
    return compaction_bytes_reclaimed_.load(std::memory_order_relaxed);
  }

  // estimate_heap_size returns the heap size needed to store n items,
  // including overheads for compacting and buffering to disk.
  //
  // This is especially useful for benchmarks and test cases, where OOM should
  // be avoided.
  static size_t estimate_heap_size(size_t n, size_t key_size,
                                   size_t value_size);

  // estimate_heap_size returns the heap size needed to store a heap of size
  // bytes, rounding up to include overheads for compacting and buffering to
  // disk. This is important because these costs vary with the number of cores.
  //
  // This is especially useful for benchmarks and test cases, where OOM should
  // be avoided.
  static size_t estimate_heap_size(size_t bytes);

private:
  // the directory that stores this database.
  Directory dir_;

  // protects cv_, active_, run_expand_, run_compact_, and run_disk_cleanup_.
  std::mutex lock_;
  std::condition_variable_any cv_;
  bool active_{true};
  bool run_expand_{false};
  bool run_compact_{false};
  bool run_disk_cleanup_{false};

  // the modules of the database. Each has a shared lifetime with this object.
  std::unique_ptr<LogletAllocator> loglet_allocator_;
  WorkerPool pool_;
  ItemAllocator item_allocator_;
  DiskWriter disk_writer_;
  ScalableMap map_;

  // runs background_worker().
  std::jthread bg_th_;

  // The highest end_lsn_exclusive of any snapshot loaded during recovery or
  // created by sync_and_snapshot(). Used to validate begin_lsn in
  // sync_and_snapshot() — begin_lsn must be 0 or <= this value to avoid
  // creating snapshot gaps that recovery cannot bridge.
  uint64_t max_snapshot_end_lsn_{0};

  // compaction activity since recovery; written only by background_worker.
  std::atomic<uint64_t> compaction_passes_{0};
  std::atomic<uint64_t> compaction_bytes_reclaimed_{0};

  // periodically updated stats; registered in register_metrics().
  struct DbMetrics {
    std::atomic<size_t> snap_bytes_out_;
    Metrics::Gauge snap_bytes_out_metric;
  };
  DbMetrics metrics_{};

  // called when memory is low.
  void notify_low_mem();

  // called when a new wal file is created; may trigger snapshots/cleanup.
  void notify_wal_grew();

  // background_worker performs periodic background work.
  void background_worker(const std::stop_token &stoken);

  static DBStatus<std::unique_ptr<Database>>
  open_internal(std::string_view path, std::optional<uint64_t> lsn,
                size_t heap_size, int bucket_order,
                size_t log_file_size_threshold);
};

inline DBStatus<std::unique_ptr<Database>>
Database::open(std::string_view path, size_t heap_size, int bucket_order,
               size_t log_file_size_threshold)
{
  return open_internal(path, std::nullopt, heap_size, bucket_order,
                       log_file_size_threshold);
}

inline DBStatus<std::unique_ptr<Database>>
Database::open_lsn(std::string_view path, uint64_t lsn, size_t heap_size,
                   int bucket_order, size_t log_file_size_threshold)
{
  return open_internal(path, {lsn}, heap_size, bucket_order,
                       log_file_size_threshold);
}

inline DBStatus<DBItem>
Database::get(std::span<const std::byte> key)
{
  assert(active_);
  return map_.get(key);
}

inline DBStatus<OwningSpan<const std::byte>>
Database::get_if_type(std::span<const std::byte> key, uint8_t type)
{
  assert(active_);
  return map_.get_if_type(key, type);
}

inline DBStatus<void>
Database::get_if_type(std::span<const std::byte> key, uint8_t type,
                      std::span<std::byte> val_out)
{
  assert(active_);
  return map_.get_if_type(key, type, val_out);
}

inline DBStatus<size_t>
Database::get_size(std::span<const std::byte> key)
{
  assert(active_);
  return map_.get_size(key);
}

inline DBStatus<uint8_t>
Database::get_type(std::span<const std::byte> key)
{
  assert(active_);
  return map_.get_type(key);
}

inline DBStatus<void>
Database::put(std::span<const std::byte> key, uint8_t type,
              std::span<const std::byte> val)
{
  assert(active_);
  return map_.put(key, type, val);
}

inline DBStatus<void>
Database::remove(std::span<const std::byte> key)
{
  assert(active_);
  return map_.remove(key);
}

inline void
Database::get_each(GetEachCallback fn)
{
  assert(active_);
  map_.for_each([&fn](ItemHeader *hdr) {
    fn(hdr->get_key(), hdr->type, hdr->get_value());
  });
}

inline void
Database::get_each_parallel(GetEachCallback fn, size_t width, WorkerPool &pool)
{
  assert(active_);
  map_.for_each_parallel(
      [&fn](ItemHeader *hdr) {
        fn(hdr->get_key(), hdr->type, hdr->get_value());
      },
      width, pool);
}

inline void
Database::for_each_location(LayoutCallback fn)
{
  assert(active_);
  map_.for_each([this, &fn](ItemHeader *hdr) {
    fn(hdr->get_key(), hdr->total_bytes(),
       loglet_allocator_->loglet_index(hdr));
  });
}

// Only safe to call while Database instance is quiescent (no ongoing calls to
// put, get, remove, etc).
inline DBStatus<uint64_t>
Database::sync()
{
  return disk_writer_.sync();
}

} // namespace hogd
