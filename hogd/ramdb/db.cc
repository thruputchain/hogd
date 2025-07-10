#include "hogd/ramdb/db.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono> // NOLINT(misc-include-cleaner)
#include <format>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "hogd/base/bits.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/perthread.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_reader.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

namespace {

constexpr int
active_cores(const WorkerPool &pool)
{
  return std::max(pool.worker_count() / kActiveBGCoreFraction, 1);
}

DBStatus<std::unique_ptr<Database>>
create_db(std::string_view path, size_t heap_size, int bucket_order)
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
Database::notify_wal_grew()
{
  std::scoped_lock g(lock_);
  run_disk_cleanup_ = true;
  cv_.notify_one();
}

void
Database::background_worker(const std::stop_token &stoken)
{
  // NOTE: This thread is needed to synchronize compaction and expansion.
  // prepare_expand() cannot be called while compaction is active.

  while (true) {
    bool expand = false;
    bool compact, disk_cleanup;
    {
      // wait for change in status or timeout.
      std::scoped_lock g(lock_);
      using namespace std::chrono_literals;
      // NOLINTNEXTLINE(misc-include-cleaner)
      cv_.wait_for(lock_, stoken, 1s,
                   [this] { return run_expand_ || run_compact_; });
      if (stoken.stop_requested())
        break;

      compact = std::exchange(run_compact_, false);
      run_expand_ = false;

      // check if the buckets need to be expanded.
      if (!active_ && map_.prepare_expand())
        expand = true;

      disk_cleanup = std::exchange(run_disk_cleanup_, false);
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

    if (disk_cleanup) {
      info("triggering disk cleanup due to wal growth");
      if (cleanup_running_.load(std::memory_order_acquire)) {
        info("cleanup already running; skipping");
      }
      else {
        if (cleanup_th_.joinable())
          cleanup_th_.join();

        // determine how much wal data there is, see if it's "too much".
        size_t log_bytes = DiskReader::log_bytes(dir_);
        bool needed = log_bytes >= disk_writer_.get_file_size_threshold();
        info("disk logs occupy {}; cleanup {}needed",
             print_iec_bytes(log_bytes, true), needed ? "" : "not ");

        if (needed) {
          cleanup_running_.store(true, std::memory_order_relaxed);

          cleanup_th_ = std::jthread{[this]() {
            // start a full snapshot so wal log files become prunable.
            auto ret = sync_and_snapshot(0);
            if (!ret) {
              warn("failed to snapshot while attempting disk cleanup: {}",
                   ret.error());
            }
            else {
              auto snap_ret = ret->second.get();
              if (!snap_ret)
                warn("failed to snapshot while attempting disk cleanup: {}",
                     snap_ret.error());
            }

            DiskReader::cleanup(dir_);

            cleanup_running_.store(false, std::memory_order_release);
          }};
        }
      }
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

  // need two loglets per compactor thread and one loglet per worker thread.
  heap_size += available_parallelism() * kLogletRegionSize * 3;

  // add disk buffering overhead.
  heap_size +=
      (1UL << scalable_shard_order(kBaseWriteHeadOrder)) * kLogletRegionSize;

  return heap_size;
}

DBStatus<std::pair<uint64_t, std::future<DBStatus<void>>>>
Database::sync_and_snapshot(uint64_t begin_lsn)
{
  DBStatus<uint64_t> ret = sync();
  if (!ret) [[unlikely]]
    return ret.error();

  uint64_t end_lsn = *ret;
  std::string path = std::format("{}-{}.snap", begin_lsn, end_lsn);
  info("begin snapshot from lsn {} -> lsn {} to {}", begin_lsn, end_lsn, path);

  if (begin_lsn > end_lsn) {
    warn("bad snapshot begin and end lsns {} to {}", begin_lsn, end_lsn);
    return DBError{DBCode::kTooBig};
  }

  std::latch setup_latch{1};
  auto promise = std::make_shared<std::promise<DBStatus<void>>>();

  std::jthread([this, path = std::move(path), begin_lsn, end_lsn, &setup_latch,
                promise] {
    debug("snapshot starting in background");
    ElapsedTimer timer{};

    struct Stats {
      size_t items;
      size_t bytes;

      Stats &operator+=(const Stats &stats)
      {
        items += stats.items;
        bytes += stats.bytes;
        return *this;
      }
    };

    PerThread<Stats> perthread_stats{};

    constexpr bool kChunkedSnapshot = false;
    constexpr size_t kMaxFileSize = kChunkedSnapshot
                                        ? kDiskLogFileThreshold
                                        : std::numeric_limits<size_t>::max();

    // only allocate one head for this writer since snapshotting is
    // single-threaded anyway.
    std::string tmp_path = std::format("__{}.tmp", path);
    DiskWriter snap_writer{*loglet_allocator_, dir_, 0};
    // start snap writer bg thread; this also configures writer so that
    // sync() at end of snapshotting writes a commit record for end_lsn - 1.
    snap_writer.init(end_lsn - 1, tmp_path, kMaxFileSize);

    auto add_item_to_snapshot = [&snap_writer, &begin_lsn, &end_lsn,
                                 &stats = perthread_stats](ItemHeader *item) {
      if (item->lsn < begin_lsn || item->lsn >= end_lsn)
        return;

      constexpr size_t kHash = 0; // irrelevant; there is only one head
      ScopedDiskWrite record = snap_writer.write_record(kHash);
      // add item to snapshot file using the lsn indicated by item->lsn.
      DBStatus<uint64_t> ret = record.modify(item, true);
      if (!ret) [[unlikely]]
        return;
      // ret LSN ignored; it is meaningless outside of redo log.

      record.reset();

      stats->items++;
      stats->bytes += item->total_bytes();
    };

    // tell map that a snapshot is ongoing.
    // if put()/remove() runs into an item that is about to be
    // overwritten that should be part of the snapshot, it triggers
    // this callback to stuff a copy of the item in the snapshot's
    // disk-bound loglets.
    map_.register_mutation_cb(add_item_to_snapshot);

    // disk writer and map are now set up, so unblock snapshot_and_sync()
    // so it can return to the user and they can resume db operations,
    // while the snapshot runs in the background.
    // after line below setup_latch access is undefined behavior.
    setup_latch.count_down();

    map_.for_each(add_item_to_snapshot);

    // snapshot is done, so we don't care about future put()/remove ops.
    map_.register_mutation_cb(nullptr);

    Stats stats{};
    {
      std::unique_lock ul = perthread_stats.lock_threads();
      perthread_stats.for_each(ul, [&stats](Stats &s) { stats += s; });
    }

    double seconds = timer.elapsed();
    info("snapshot took {:.3f} s (not including disk sync)", seconds);
    info("...total items snapshot {}", stats.items);
    info("...total bytes snapshot {}", print_iec_bytes(stats.bytes, true));
    info("...snapshot rate {}/s", print_iec_bytes(stats.bytes / seconds), true);

    ElapsedTimer snap_sync_timer{};

    DBStatus<uint64_t> ret = snap_writer.sync();
    if (!ret) [[unlikely]] {
      warn("snapshot sync failed");
      promise->set_value(ret.transform([](auto &e) {}));
      return;
    }

    // atomically rename snapshot files to prevent recovery from trying to
    // recover partially written snapshots.
    ErrnoStatus<void> r = dir_.rename(tmp_path, path);
    if (!r) {
      warn("snapshot atomic rename from {} to {} failed", tmp_path, path);
      promise->set_value(ret.error());
      return;
    }
    // sync the dir to avoid losing the updated dentry (thanks abelay!).
    DBStatus<void> dirsync_ret = dir_.sync().transform_error(to_db_error);
    if (!dirsync_ret) {
      promise->set_value(dirsync_ret.error());
      return;
    }

    info("syncing snapshot file to storage took {:.3f} s",
         snap_sync_timer.elapsed());

    promise->set_value({});
  }).detach();
  // TODO(stutsman): should we keep track of the ongoing snapshot thread so
  // the DB destructor can await any ongoing snapshots to avoid crashes/UB on DB
  // shutdown? A problem with doing that is that the stop token would have to be
  // integrated into map_.for_each.

  // wait for snapshotter thread to get writer initialized and map informed of
  // snapshot; after that the user can resume db operations.
  setup_latch.wait();

  return std::make_pair(end_lsn, promise->get_future());
}

DBStatus<std::unique_ptr<Database>>
Database::open_internal(std::string_view path, std::optional<uint64_t> lsn,
                        size_t heap_size, int bucket_order,
                        size_t log_file_size_threshold)
{
  DBStatus<std::unique_ptr<Database>> ret =
      create_db(path, heap_size, bucket_order);
  if (!ret)
    return ret.error();
  std::unique_ptr<Database> db = std::move(ret.value());
  db->pool_.adjust_workers(active_cores(db->pool_));

  DiskReader r(*db->loglet_allocator_, db->map_);
  // load up to a specific LSN or everything if no lsn limit given.
  DBStatus<uint64_t> load_ret = r.load(db->dir_, lsn);
  if (!load_ret) {
    error("failed to load database: {}", load_ret.error());
    return load_ret.error();
  }

  if (lsn && *load_ret != *lsn) {
    error("failed to recover up through specified lsn");
    return DBError{DBCode::kCorruption};
  }

  if (*load_ret == 0)
    info("no existing database found; creating a new database.");
  else
    info("loaded up to LSN {}, {} items", *load_ret, db->map_.size());
  db->disk_writer_.init(*load_ret, "{}.log", log_file_size_threshold);
  return db;
}

} // namespace hogd
