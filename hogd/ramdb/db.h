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
#include "hogd/ramdb/heat_sketch.h"
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

class Database;

// configure_objectier_from_env turns on popularity-driven consolidation if
// HOGD_OBJECTIER names the number of loglets one pass may relocate, and
// optionally reads the hot-set share from HOGD_OBJECTIER_FRACTION.
//
// Off by default, and an environment variable rather than a config field for the
// same reason RAMDB_HEAP_FILE is: it costs background CPU and adds a counter to
// the read path, so no deployment should get it without asking. Shared by every
// entry point that opens a Database so they cannot drift.
void configure_objectier_from_env(Database &db);

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

  // enable_heat_tracking allocates the read-hotness sketch. Nothing in ramdb
  // feeds it: repeat accesses to an item within a block are served from
  // BlockTX's lock cache and never reach the storage engine, so a counter here
  // would see one read per item per block and miss frequency entirely. Callers
  // above that cache feed it via record_heat().
  void enable_heat_tracking(int width_order = 20, size_t rows = 4)
  {
    heat_ = std::make_unique<HeatSketch>(width_order, rows);
  }

  // age_heat halves every counter, so the sketch tracks what is hot rather than
  // what was ever hot. The caller owns the epoch.
  void age_heat()
  {
    if (heat_)
      heat_->halve();
  }

  size_t heat_bytes() const { return heat_ ? heat_->size_bytes() : 0; }

  // evict_heap drops the heap's resident pages. Only meaningful with a
  // file-backed heap; used to give two layouts the same cold start before
  // comparing their fault counts.
  void evict_heap() { loglet_allocator_->evict_region(); }

  // evict_heap_except emulates a fast tier holding exactly the loglets set in
  // keep, by dropping the resident pages of all the others. See
  // LogletAllocator::evict_except.
  void evict_heap_except(const std::vector<bool> &keep)
  {
    loglet_allocator_->evict_except(keep);
  }

  // heat_enabled lets a caller skip building a key it would only throw away.
  bool heat_enabled() const { return heat_ != nullptr; }

  // record_heat notes one logical access to key. Callers above a caching layer
  // should use this rather than relying on the ramdb read path: hogdb serves
  // repeat accesses to an item within a block from BlockTX's lock cache, so the
  // storage engine sees one read per item per block and access *frequency* is
  // invisible to it.
  //
  // The key must be the same bytes the item is stored under, since that is what
  // a consolidation classifier will look up.
  void record_heat(std::span<const std::byte> key)
  {
    if (heat_)
      heat_->touch(ScalableMap::hash_key(key));
  }

  // heat_estimate returns the sketch's approximate access count for a key.
  uint8_t heat_estimate(std::span<const std::byte> key) const
  {
    return heat_ ? heat_->estimate(ScalableMap::hash_key(key)) : 0;
  }

  // heat_threshold returns the access count that classifies roughly the hottest
  // hot_fraction of tracked keys as hot, letting a caller size its hot set to a
  // fast tier rather than pick a count. See HeatSketch::hot_threshold. Returns
  // 1 when no sketch exists, i.e. any recorded access counts as hot.
  uint8_t heat_threshold(double hot_fraction) const
  {
    return heat_ ? heat_->hot_threshold(hot_fraction) : 1;
  }

  // consolidate reorganizes loglets so each holds items of a single
  // temperature. See ItemAllocator::consolidate.
  //
  // Safe to run concurrently with put()/remove(), on the same grounds compact()
  // is: relocation re-checks the item pointer under the bucket lock, and the
  // hotness walk touches only immutable key bytes plus the storage byte, which
  // it reads atomically. A loglet on the compactable list is no longer a write
  // head, so its bump pointer is frozen while the walk runs.
  //
  // What is *not* safe concurrently is reaching the newest items, which sit in
  // partly filled write heads -- see roll_over_write_heads().
  size_t consolidate(const std::function<bool(ItemHeader *)> &is_hot,
                     float min_mixedness, size_t max_victims = 0)
  {
    return item_allocator_.consolidate(is_hot, min_mixedness, max_victims);
  }

  // roll_over_write_heads makes the newest items reachable by consolidate() and
  // compact(). A per-thread write head joins the compactable set only when it
  // fills, so without this up to one loglet per writing thread of the most
  // recently written data can never be selected.
  //
  // PRECONDITION: no put() or remove() may overlap this call -- it moves the
  // heads those paths allocate from. That makes it the one part of a
  // consolidation pass that needs quiescence, which is why it is separate: a
  // caller that cannot stop writes can still consolidate everything else.
  void roll_over_write_heads() { item_allocator_.roll_over_heads(); }

  // enable_auto_consolidation sets the policy consolidate_incremental() uses.
  // hot_fraction sizes the hot set (see heat_threshold), max_victims bounds a
  // single pass, and min_mixedness is the selection threshold. Requires the heat
  // sketch: without it there is no classifier and nothing to route by.
  //
  // min_mixedness must be above zero for a repeating trigger, and defaults to
  // kAutoConsolidateMinMixedness rather than the 0 a one-shot pass uses -- see
  // that constant for why, and for why the default lives there. Measured over 20
  // block boundaries at 2 loglets each: 0 reaches hit 0.7534, 0.05 reaches
  // 0.8979, 0.10 reaches 0.9045 and then selects nothing further.
  void enable_auto_consolidation(
      double hot_fraction, size_t max_victims,
      float min_mixedness = kAutoConsolidateMinMixedness)
  {
    auto_hot_fraction_ = hot_fraction;
    auto_max_victims_ = max_victims;
    auto_min_mixedness_ = min_mixedness;
  }

  // consolidate_incremental runs one bounded consolidation pass classified by
  // the heat sketch. Returns the loglets relocated, 0 if unconfigured or if
  // another pass is already running.
  //
  // Bounded because the cost is ~23 ms per loglet: reaching a good layout is a
  // matter of accumulating passes over successive blocks rather than finishing
  // one, and a bounded pass never monopolizes the background thread.
  //
  // Does not roll the write heads over, so it inherits consolidate()'s
  // concurrency: safe alongside put()/remove(), which is what lets it run on
  // background_worker rather than on a committing thread. The newest items are
  // therefore not candidates until their head fills on its own.
  size_t consolidate_incremental()
  {
    if (auto_max_victims_ == 0 || !heat_)
      return 0;

    // Skip rather than queue when a pass is in flight. Passes are
    // opportunistic, and two overlapping ones would race on the single pair of
    // relocation destinations the allocator keeps for consolidation.
    std::unique_lock pass(consolidate_lock_, std::try_to_lock);
    if (!pass.owns_lock())
      return 0;

    uint8_t thresh = heat_threshold(auto_hot_fraction_);
    auto is_hot = [this, thresh](ItemHeader *item) {
      return heat_->estimate(ScalableMap::hash_key(item->get_key())) >= thresh;
    };
    size_t moved = consolidate(is_hot, auto_min_mixedness_, auto_max_victims_);
    consolidation_passes_.fetch_add(1, std::memory_order_relaxed);
    consolidation_loglets_.fetch_add(moved, std::memory_order_relaxed);
    return moved;
  }

  // notify_block_committed asks for a consolidation pass at the next
  // opportunity. Cheap by design -- a flag and a notify -- because it is called
  // from the commit path, where ~23 ms per loglet of relocation must not land.
  // background_worker does the work.
  //
  // A block boundary is the natural cadence: it is when the store has just
  // finished a batch of writes, and it arrives whether or not anything is
  // fragmented, which demand-driven compaction does not.
  void notify_block_committed()
  {
    if (auto_max_victims_ == 0)
      return;
    std::scoped_lock g(lock_);
    run_consolidate_ = true;
    cv_.notify_one();
  }

  uint64_t consolidation_passes() const
  {
    return consolidation_passes_.load(std::memory_order_relaxed);
  }
  uint64_t consolidation_loglets() const
  {
    return consolidation_loglets_.load(std::memory_order_relaxed);
  }

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
  bool run_consolidate_{false};

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

  // approximate read-hotness tracking, when enabled.
  std::unique_ptr<HeatSketch> heat_;

  // compaction activity since recovery; written only by background_worker.
  std::atomic<uint64_t> compaction_passes_{0};
  std::atomic<uint64_t> compaction_bytes_reclaimed_{0};

  // consolidate_incremental() policy and counters. max_victims of 0 disables it,
  // which is the default: consolidation costs CPU at a quiescent point and no
  // caller should get it without asking.
  double auto_hot_fraction_{0.2};
  size_t auto_max_victims_{0};
  float auto_min_mixedness_{0.0F};
  // serializes consolidation passes; separate from lock_, which guards the
  // background_worker wake-up state and must not be held for the duration of a
  // pass.
  std::mutex consolidate_lock_;
  std::atomic<uint64_t> consolidation_passes_{0};
  std::atomic<uint64_t> consolidation_loglets_{0};

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
