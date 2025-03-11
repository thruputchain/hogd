#include "hogd/ramdb/db.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono> // NOLINT(misc-include-cleaner)
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_reader.h"
#include "hogd/ramdb/io.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

namespace {

constexpr int
active_cores(const WorkerPool &pool)
{
  return std::max(pool.worker_count() / kActiveBGCoreFraction, 1);
}

} // namespace

DBStatus<std::unique_ptr<Database>>
Database::open(const std::string &path, size_t heap_size, int bucket_order)
{
  // NOTE: all variables with the _order suffix means they are log2(n).

  // the heap size must be in increments of loglet size.
  heap_size = align_up(heap_size, kLogletRegionSize);

  // calculate the number of locks based on the available cores.
  size_t cores = std::thread::hardware_concurrency();
  int cores_order = std::countr_zero(std::bit_ceil(cores));
  int lock_order = kBaseLockOrder + cores_order;

  // the starting bucket order must be at least as large as the lock order.
  bucket_order = std::max(lock_order, bucket_order);

  // calculate the number of write heads based on the available cores.
  int write_head_order = kBaseWriteHeadOrder + cores_order;

  // open the database directory.
  ErrnoStatus<Directory> dir = Directory::open(path);
  if (!dir) {
    error("couldn't create directory: {}", dir.error());
    return to_db_error(dir.error());
  }

  // create the database.
  auto db = std::make_unique<Database>(*dir, heap_size, lock_order,
                                       bucket_order, write_head_order);
  db->pool_.adjust_workers(active_cores(db->pool_));

  // load the existing log if it exists.
  DiskReader r(*db->loglet_allocator_, db->map_);
  DBStatus<uint64_t> lsn = r.load(*dir);
  if (!lsn) {
    error("failed to load database: {}", lsn.error());
    return lsn.error();
  }
  if (*lsn == 0)
    debug("no existing database found; creating a new database.");
  else
    debug("loaded up to LSN {}, {} items", *lsn, db->map_.size());
  db->disk_writer_.init(*lsn);

  // print configuration for debugging.
  debug("opening DB at '{}':", path);
  debug("...heap_size {}, cores {}", print_iec_bytes(heap_size), cores);
  debug("...locks {}, buckets {}, write_heads {}", (1UL << lock_order),
        (1UL << bucket_order), (1ULL << write_head_order));

  return db;
}

void
Database::set_active(bool yes)
{
  // update active state and notify bg worker.
  {
    std::scoped_lock g(lock_);
    active_ = yes;
    if (!active_) {
      run_expand_ = true;
      cv_.notify_one();
    }
  }

  // set the worker pool limit.
  int cores = yes ? active_cores(pool_) : pool_.worker_count();
  pool_.adjust_workers(cores);
}

void
Database::notify_low_mem()
{
  std::scoped_lock g(lock_);
  run_compact_ = true;
  cv_.notify_one();
}

void
Database::background_worker(const std::stop_token &stoken)
{
  // NOTE: This thread is needed to synchronize compaction and expansion.
  // prepare_expand() cannot be called while compaction is active.

  while (true) {
    bool expand = false;
    bool compact;
    {
      // wait for change in status or timeout.
      std::scoped_lock g(lock_);
      using namespace std::chrono_literals;
      // NOLINTNEXTLINE(misc-include-cleaner)
      cv_.wait_for(lock_, stoken, 1s,
                   [this] { return run_expand_ || run_compact_; });
      if (stoken.stop_requested())
        return;
      compact = run_compact_;
      run_expand_ = false;

      // check if the buckets need to be expanded.
      if (!active_ && map_.prepare_expand())
        expand = true;
    }

    // run item compactor if needed.
    if (compact) {
      size_t freed_bytes = item_allocator_.compact(kMinCompactUtilization);
      if (freed_bytes == 0) {
        // compact all loglets if threshold didn't work.
        warn("still out of memory; compacting all loglets (no threshold)");
        freed_bytes = item_allocator_.compact(1.0F);
      }
      bool not_oom = freed_bytes > 0 || disk_writer_.bytes_buffered() > 0;
      loglet_allocator_->low_mem_done(not_oom);
    }

    // run bucket expansion if needed.
    if (expand)
      map_.do_expand();

    // print statistics about memory use.
    if (!compact && !expand) {
      loglet_allocator_->print_stats();
      disk_writer_.print_stats();
    }
  }
}

size_t
Database::estimate_heap_size(size_t n, size_t key_size, size_t value_size)
{
  // calculate size per item.
  size_t item_size = sizeof(ItemHeader) + key_size + value_size;
  assert(item_size < kBigItemThreshold);
  item_size = align_up(item_size, alignof(ItemHeader));

  // determine how much heap is needed to store the items, including
  // fragmentation at the end of each loglet.
  size_t heap_size = align_up(n * item_size, kLogletRegionSize);
  heap_size += (heap_size / kLogletRegionSize + 1) * kBigItemThreshold;
  heap_size = align_up(heap_size, kLogletRegionSize);

  // determine number of cores.
  size_t cores = std::jthread::hardware_concurrency();
  int cores_order = std::countr_zero(std::bit_ceil(cores));

  // add the compactor overhead.
  heap_size += cores * kLogletRegionSize * 2;

  // add disk buffering overhead.
  heap_size += (1UL << (cores_order + kBaseWriteHeadOrder)) * kLogletRegionSize;

  return heap_size;
}

} // namespace hogd
