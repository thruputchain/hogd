#include "hogd/ramdb/db.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono> // NOLINT(misc-include-cleaner)
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "hogd/base/bits.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_reader.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

namespace {

constexpr int
active_cores(const WorkerPool &pool)
{
  return std::max(pool.worker_count() / kActiveBGCoreFraction, 1);
}

DBStatus<std::unique_ptr<Database>>
create_db(const std::string &path, size_t heap_size, int bucket_order)
{
  // NOTE: all variables with the _order suffix means they are log2(n).

  // the heap size must be in increments of loglet size.
  heap_size = align_up(heap_size, kLogletRegionSize);

  // calculate the number of locks based on the available cores.
  int lock_order = scalable_shard_order(kBaseLockOrder);

  // the starting bucket order must be at least as large as the lock order.
  bucket_order = std::max(lock_order, bucket_order);

  // calculate the number of write heads based on the available cores.
  int write_head_order = scalable_shard_order(kBaseWriteHeadOrder);

  // print configuration for debugging.
  debug("opening DB at '{}':", path);
  debug("...heap_size {}, cores {}", print_iec_bytes(heap_size),
        available_parallelism());
  debug("...locks {}, buckets {}, write_heads {}", (1UL << lock_order),
        (1UL << bucket_order), (1ULL << write_head_order));

  // open the database directory.
  ErrnoStatus<Directory> dir = Directory::open(path);
  if (!dir) {
    error("couldn't create directory: {}", dir.error());
    return to_db_error(dir.error());
  }

  // create the database.
  return std::make_unique<Database>(*dir, heap_size, lock_order, bucket_order,
                                    write_head_order);
}

} // namespace

DBStatus<std::unique_ptr<Database>>
Database::open(const std::string &path, size_t heap_size, int bucket_order)
{
  DBStatus<std::unique_ptr<Database>> ret =
      create_db(path, heap_size, bucket_order);
  if (!ret)
    return ret.error();
  std::unique_ptr<Database> db = std::move(ret.value());
  db->pool_.adjust_workers(active_cores(db->pool_));

  // load all the LSNs that are available and valid.
  DiskReader r(*db->loglet_allocator_, db->map_);
  DBStatus<uint64_t> lsn = r.load(db->dir_);
  if (!lsn) {
    error("failed to load database: {}", lsn.error());
    return lsn.error();
  }
  if (*lsn == 0)
    debug("no existing database found; creating a new database.");
  else
    debug("loaded up to LSN {}, {} items", *lsn, db->map_.size());
  db->disk_writer_.init(*lsn);
  return db;
}

DBStatus<std::unique_ptr<Database>>
Database::open_lsn(const std::string &path, uint64_t lsn, size_t heap_size,
                   int bucket_order)
{
  DBStatus<std::unique_ptr<Database>> ret =
      create_db(path, heap_size, bucket_order);
  if (!ret)
    return ret.error();
  std::unique_ptr<Database> db = std::move(ret.value());
  db->pool_.adjust_workers(active_cores(db->pool_));

  // load up to a specific LSN.
  DiskReader r(*db->loglet_allocator_, db->map_);
  DBStatus<uint64_t> load_ret = r.load(db->dir_, lsn);
  if (!load_ret) {
    error("failed to load database: {}", load_ret.error());
    return load_ret.error();
  }
  assert(*load_ret == lsn);
  db->disk_writer_.init(lsn);
  return db;
}

DBStatus<uint64_t>
Database::check(const std::string &path, size_t heap_size)
{
  DBStatus<std::unique_ptr<Database>> ret = create_db(path, heap_size, 0);
  if (!ret)
    return ret.error();
  std::unique_ptr<Database> db = std::move(ret.value());

  // Check the integrity of the database.
  DiskReader r(*db->loglet_allocator_, db->map_);
  DBStatus<uint64_t> valid_lsn = r.verify(db->dir_);
  if (!valid_lsn) {
    error("failed to check database: {}", valid_lsn.error());
    return valid_lsn.error();
  }
  return *valid_lsn;
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
    bool snapshot;
    {
      // wait for change in status or timeout.
      std::scoped_lock g(lock_);
      using namespace std::chrono_literals;
      // NOLINTNEXTLINE(misc-include-cleaner)
      cv_.wait_for(lock_, stoken, 1s,
                   [this] { return run_expand_ || run_compact_; });
      if (stoken.stop_requested())
        return;

      snapshot = snapshotter_.has_value();
      compact = run_compact_;
      run_expand_ = false;

      // check if the buckets need to be expanded.
      if (!active_ && map_.prepare_expand())
        expand = true;
    }

    if (snapshot) {
      snapshotter_->snapshot();
      snapshotter_.reset();
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

    // return idle memory to the OS.
    loglet_allocator_->reclaim_idle_memory();

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

std::pair<DBStatus<uint64_t>, std::future<DBStatus<void>>>
Database::sync_and_snapshot(const std::string &path, uint64_t begin_lsn)
{
  DBStatus<uint64_t> ret = sync();
  if (!ret) [[unlikely]]
    return {ret.error(), {}};

  uint64_t end_lsn = ret.value();

  info("begin snapshot from lsn {} -> lsn {} to {}", begin_lsn, end_lsn, path);

  if (begin_lsn > end_lsn) {
    warn("bad snapshot begin and end lsns {} to {}", begin_lsn, end_lsn);
    return {DBError{DBCode::kTooBig}, {}};
  }

  std::future<DBStatus<void>> future{};
  {
    std::scoped_lock g(lock_);

    if (snapshotter_) {
      warn("tried to start a snapshot while one was already ongoing");
      return {DBError{DBCode::kExists}, {}};
    }

    constexpr bool kChunkedSnapshot = false;
    constexpr size_t kMaxFileSize = kChunkedSnapshot
                                        ? kDiskLogFileThreshold
                                        : std::numeric_limits<size_t>::max();

    // put()s with lsn s.t. begin_lsn <= lsn <= end_lsn
    snapshotter_.emplace(pool_, map_, item_allocator_, begin_lsn, end_lsn,
                         *loglet_allocator_, dir_, path, kMaxFileSize,
                         write_head_order_);

    future = snapshotter_->get_future();

    // trigger background worker which serializes the work with compacting
    cv_.notify_one();
  }

  return {{end_lsn}, std::move(future)};
}

} // namespace hogd
