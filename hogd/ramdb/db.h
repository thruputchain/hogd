// db.h - the main interface to the database

#pragma once

#include <memory>
#include <thread>

#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/item_allocator.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

// Database provides the interface for using ramdb. Normally, you should only
// interact with this class.
class Database {
public:
  // open is a factory method to create a database.
  static DBStatus<std::unique_ptr<Database>>
  open(const std::string &path, size_t heap_size, int bucket_order = 0);

  Database(Directory dir, size_t heap_size, int lock_order, int bucket_order,
           int write_head_order)
    : loglet_allocator_(
          LogletAllocator::create(heap_size,
                                  std::thread::hardware_concurrency(),
                                  [this] { notify_low_mem(); })),
      item_allocator_(*loglet_allocator_, pool_, &map_),
      disk_writer_(*loglet_allocator_, std::move(dir), write_head_order),
      map_(item_allocator_, disk_writer_, lock_order, bucket_order),
      bg_th_(
          [this](const std::stop_token &stoken) { background_worker(stoken); })
  {}
  ~Database() { bg_th_.request_stop(); }

  // Disable move and copy.
  Database(Database &&db) = delete;
  Database &operator=(Database &&db) = delete;
  Database(const Database &db) = delete;
  Database &operator=(const Database &db) = delete;

  // get finds an item for a given key.
  DBStatus<DBItem> get(std::span<const std::byte> key);

  // get_if_type finds an item of a specific type for a given key. If the
  // type or the size of val_out do not match exactly, it will return
  // DBCode::kTypeMismatch.
  DBStatus<void> get_if_type(std::span<const std::byte> key, uint8_t type,
                             std::span<std::byte> val_out);

  // put inserts an item.
  DBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                     std::span<const std::byte> val);

  // remove deletes an item. It is not considered an error if the item does not
  // exist.
  DBStatus<void> remove(std::span<const std::byte> key);

  // sync writes pending items to disk. Any calls to put() or remove() performed
  // before this call are guaranteed to be recoverable after a failure.
  DBStatus<uint64_t> sync();

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
  std::unique_ptr<LogletAllocator> loglet_allocator_;
  WorkerPool pool_;
  ItemAllocator item_allocator_;
  DiskWriter disk_writer_;
  ScalableMap map_;

  std::mutex lock_; // protects cv_, active_, run_expand_, and run_compact_.
  std::condition_variable_any cv_;
  bool active_{true};
  bool run_expand_{false};
  bool run_compact_{false};

  // runs background_worker().
  std::jthread bg_th_;

  // called when memory is low.
  void notify_low_mem();
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

inline DBStatus<uint64_t>
Database::sync()
{
  return disk_writer_.sync();
}

} // namespace hogd
