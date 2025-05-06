// db.h - the main interface to the database

#pragma once

#include <functional>
#include <memory>
#include <thread>

#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/item_allocator.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"
#include "hogd/ramdb/snapshotter.h"

namespace hogd {

// GetEachCallback is used by get_each(). Arguments are key, type, and value.
using GetEachCallback = std::function<void(std::span<const std::byte>, uint8_t,
                                           std::span<const std::byte>)>;

// Database provides the interface for using ramdb. Normally, you should only
// interact with this class.
class Database {
public:
  //
  // Factory methods: Use these to initialize a database.
  //

  // open opens a database at path.
  static DBStatus<std::unique_ptr<Database>>
  open(const std::string &path, size_t heap_size, int bucket_order = 0);

  // open_lsn opens a database at path and loads only up to a specific LSN.
  static DBStatus<std::unique_ptr<Database>> open_lsn(const std::string &path,
                                                      uint64_t lsn,
                                                      size_t heap_size,
                                                      int bucket_order = 0);

  // check checks the integrity of a database. It returns the highest valid LSN.
  static DBStatus<uint64_t> check(const std::string &path, size_t heap_size);

  Database(Directory dir, size_t heap_size, int lock_order, int bucket_order,
           int write_head_order)
    : dir_(std::move(dir)),
      loglet_allocator_(LogletAllocator::create(heap_size,
                                                available_parallelism(),
                                                [this] { notify_low_mem(); })),
      pool_(available_parallelism()),
      item_allocator_(*loglet_allocator_, pool_, &map_),
      disk_writer_(*loglet_allocator_, dir_, "{}.log", kDiskLogFileThreshold,
                   write_head_order),
      write_head_order_{write_head_order},
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

  // put inserts an item.
  DBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                     std::span<const std::byte> val);

  // remove deletes an item. It is not considered an error if the item does not
  // exist.
  DBStatus<void> remove(std::span<const std::byte> key);

  // get_each reads every item and provides its key, type, and value to a
  // callback.
  void get_each(GetEachCallback fn);

  // get_each_parallel reads every item and provides its key, type, and value to
  // a callback that runs in parallel on a worker pool.
  void get_each_parallel(GetEachCallback fn,
                         size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool());

  // sync writes pending items to disk. Any calls to put() or remove() performed
  // before this call are guaranteed to be recoverable after a failure.
  // returns an lsn that can be passed to snapshot() to indicate which
  // items should be included in a snapshot.
  DBStatus<uint64_t> sync();

  // write all of the state changes that happened at after begin_lsn (which is 0
  // or an lsn previously returned by sync() but before end_lsn (which is an
  // lsn previously returned by a different call to sync()).
  // begin_lsn must always be an lsn from an earlier call to sync() than
  // end_lsn.
  DBStatus<void> snapshot(const std::string &path, uint64_t begin_lsn,
                          uint64_t end_lsn);

  // set_active indicates to the database that it is in a state of activity (if
  // yes is true) or inactivity (if yes is false).  When inactive, items should
  // not be accessed (e.g., get(), put(), etc.); this allows the database to
  // run background activities that could disrupt performance and may not be
  // thread safe.
  void set_active(bool yes);

  // estimate_heap_size returns the heap size needed to store n items,
  // including overheads for compacting and buffering to disk.
  //
  // This is especially useful for benchmarks and test cases, where OOM should
  // be avoided.
  static size_t estimate_heap_size(size_t n, size_t key_size,
                                   size_t value_size);

private:
  // the directory that stores this database.
  Directory dir_;

  // the modules of the database. Each has a shared lifetime with this object.
  std::unique_ptr<LogletAllocator> loglet_allocator_;
  WorkerPool pool_;
  ItemAllocator item_allocator_;
  DiskWriter disk_writer_;
  int write_head_order_;
  ScalableMap map_;

  // protects cv_, active_, run_expand_, run_compact_, and
  // snapshotter_.
  std::mutex lock_;
  std::condition_variable_any cv_;
  bool active_{true};
  bool run_expand_{false};
  bool run_compact_{false};
  std::optional<Snapshotter> snapshotter_;

  // runs background_worker().
  std::jthread bg_th_;

  // called when memory is low.
  void notify_low_mem();

  // helper used by background worked in snapshotting.
  DBStatus<void> snapshot_one(UniqueLoglet &src);

  // background_worker performs periodic background work.
  void background_worker(const std::stop_token &stoken);
};

inline DBStatus<DBItem>
Database::get(std::span<const std::byte> key)
{
  assert(active_);
  return map_.get(key);
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

// Only safe to call while Database instance is quiescent (no ongoing calls to
// put, get, remove, etc).
inline DBStatus<uint64_t>
Database::sync()
{
  // We assume no ongoing put() operations, etc. Rolling over the per-thread log
  // heads here ensures that all completed modifications are part of compactable
  // loglets and aren't sitting in the per-thread heads. This is important
  // since the snapshotter only traverses compactable loglets.
  // This also establishes the sensible invariant that every synced update
  // is in compactable loglets and not part of an active log head mixed with
  // unsynced updates.
  item_allocator_.roll_heads();

  return disk_writer_.sync();
}

} // namespace hogd
