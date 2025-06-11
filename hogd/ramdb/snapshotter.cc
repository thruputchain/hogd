#include "hogd/ramdb/snapshotter.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/item_allocator.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

bool
Snapshotter::snapshot()
{
  if (!timer_)
    start();

  if (check_completions()) {
    finish();
    return true;
  }

  return false;
}

void
Snapshotter::start()
{
  timer_.emplace();

  debug("snapshotter issuing work to pool");
  completions_.reserve(lls_.size());
  for (UniqueLoglet &ll : lls_) {
    completions_.emplace_back(pool_.submit(
        [this, &ll] mutable { return snapshot_one(std::move(ll)); }));
  }
  completions_it_.emplace(completions_.begin());
}

bool
Snapshotter::check_completions()
{
  if (!completions_it_) {
    error("called when snapshot was not in progress");
    return true;
  }

  for (auto &it = *completions_it_; it != completions_.end(); ++it) {
    if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return false;

    DBStatus r = it->get();
    // if !aggregated_stats_ then it is already set to some error, so the return
    // value of this future can be ignored (even if it is success the overall
    // snapshot should fail).
    if (aggregated_stats_) {
      if (r) {
        // If no prior failures and this loglet snapshot didn't fail, merge
        // stats.
        *aggregated_stats_ += r.value();
      }
      else {
        // Otherwise remember the error.
        aggregated_stats_ = r;
      }
    }
  }

  return true;
}

void
Snapshotter::finish()
{
  warn("Finishing snapshot");
  SnapshotStats &stats = *aggregated_stats_;

  // Ensure no ongoing or later put()s see the snapshot in progress.
  map_.register_in_progress_snapshot(0, 0, nullptr);

  // Aggregate the stats from the callbacks from the map.
  std::unique_lock ul = perthread_stats_.lock_threads();
  perthread_stats_.for_each(ul, [&stats](SnapshotStats &s) { stats += s; });

  double seconds = timer_ ? timer_->elapsed() : 0;
  info("snapshot took {:.3f} s (not including disk sync)", seconds);

  info("snapshot complete");
  info("...live items {}", stats.live_items);
  info("...live data {}", print_iec_bytes(stats.live_bytes, true));
  info("...total items scanned {}", stats.items);
  info("...total bytes scanned {}", print_iec_bytes(stats.bytes, true));
  info("...scan rate {:.1f} MiB/s", stats.bytes / seconds / (1LU << 20));
  info("...total items forwarded from ongoing put() ops {}",
       stats.forwarded_items);
  info("...total bytes forwarded from ongoing put() ops {}",
       print_iec_bytes(stats.forwarded_bytes, true));

  ElapsedTimer snap_sync_timer{};

  DBStatus<void> ret = sync();
  if (!ret.has_value()) {
    warn("sync before snapshot failed");
    promise_.set_value(ret);
    return;
  }

  debug("syncing snapshot file to storage took {:.3f} s",
        snap_sync_timer.elapsed());

  promise_.set_value({});
}

DBStatus<detail::SnapshotStats>
Snapshotter::snapshot_one(UniqueLoglet src)
{
  SnapshotStats stats{};
  stats.bytes = src->size();

  // write out items in this loglet.
  for (ItemHeader &h : *src) {
    stats.items++;
    ItemHeader *src_item = std::addressof(h);

    // optimization: skip dead items that can be detected without locking.
    auto storage = std::atomic_ref<ItemStorage>(src_item->storage);
    if (storage.load(std::memory_order_acquire) == ItemStorage::kTombstone)
      continue;

    // only include items in the lsn range of the snapshot.
    // Note that DB if sync() returns an lsn, all items covered by that
    // sync() call have lsns *less than* the returned lsn.
    // Which is end_lsn_ is non-inclusive.
    // relaxed here is sufficient. It doesn't matter if we see a stale lsn here,
    // since an item can only go from included to excluded from the snapshot lsn
    // range. If we read a stale lsn, the double check in the modify_locked
    // block will correct it.
    auto src_lsn = std::atomic_ref<uint64_t>(src_item->lsn)
                       .load(std::memory_order_relaxed);
    if (src_lsn < begin_lsn_ || src_lsn >= end_lsn_)
      continue;

    // try to move the item to the new loglet.
    DBStatus<uint64_t> ret{};
    [[maybe_unused]] bool found =
        map_.modify_locked(src_item->get_key(), [this, src_item, &stats,
                                                 &storage,
                                                 &ret](ItemHeader ** /* hptr */,
                                                       size_t hash) mutable {
          // recheck if the item became dead before the lock
          // was held.
          if (storage.load(std::memory_order_relaxed) ==
              ItemStorage::kTombstone) [[unlikely]] {
            return;
          }

          // recheck if item lsn is still supposed to be part of the snapshot.
          // if not, a put() operation overwrote the item (and it put the
          // original version in the snapshot).
          auto src_lsn = std::atomic_ref<uint64_t>(src_item->lsn)
                             .load(std::memory_order_relaxed);
          if (src_lsn < begin_lsn_ || src_lsn >= end_lsn_)
            return;

          ScopedDiskWrite record = disk_writer_.write_record(hash);
          ret = record.modify(src_item);
          if (!ret) [[unlikely]]
            return;
          // ret LSN ignored; it is meaningless outside of
          // redo log.

          record.reset();

          // point the map to the new location (bucket lock
          // is held).
          stats.live_items++;
          stats.live_bytes += src_item->total_bytes();
        });
    if (!ret) [[unlikely]] {
      item_allocator_.give_to_compactor(std::move(src));
      return ret.error();
    }
    assert(found ||
           storage.load(std::memory_order_relaxed) == ItemStorage::kTombstone);
  }

  // Since snapshotting is done on this loglet, let the compactor have it
  // back so that it can make some progress on cleaning while the
  // snapshotter is running.
  item_allocator_.give_to_compactor(std::move(src));

  return stats;
}

} // namespace hogd
