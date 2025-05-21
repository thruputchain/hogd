#include "hogd/ramdb/snapshotter.h"

#include <atomic>
#include <cassert>
#include <future>
#include <memory>
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

DBStatus<void>
Snapshotter::snapshot()
{
  ElapsedTimer timer{};
  DBStatus<SnapshotStats> stats{SnapshotStats{}};

  constexpr bool kSync = false;

  debug("snapshotter issuing work to pool");
  std::vector<std::future<DBStatus<SnapshotStats>>> completions{};
  completions.reserve(lls_.size());
  for (UniqueLoglet &ll : lls_) {
    completions.emplace_back(pool_.submit(
        [this, &ll] mutable { return snapshot_one(std::move(ll)); }));
    if (kSync) {
      DBStatus<SnapshotStats> r = completions.back().get();
      if (!r.has_value()) {
        return r.error();
      }
      stats.value() += r.value();
    }
  }

  if (!kSync) {
    for (std::future<DBStatus<SnapshotStats>> &f : completions) {
      DBStatus<SnapshotStats> r = f.get();
      if (!r.has_value()) {
        return r.error();
      }
      stats.value() += r.value();
    }
  }

  // Ensure no ongoing or later put()s see the snapshot in progress.
  map_.register_in_progress_snapshot(0, 0, nullptr);

  // Aggregate the stats from the callbacks from the map.
  std::unique_lock ul = perthread_stats_.lock_threads();
  perthread_stats_.for_each(ul, [this, &stats](SnapshotStats &s) {
    stats.value() += s;
  });

  double seconds = timer.elapsed();
  info("snapshot took {:.3f} s (not including disk sync)", seconds);

  info("snapshot complete");
  info("...live items {}", stats->live_items);
  info("...live data {}", print_iec_bytes(stats->live_bytes, true));
  info("...total items scanned {}", stats->items);
  info("...total bytes scanned {}", print_iec_bytes(stats->bytes, true));
  info("...scan rate {:.1f} MiB/s", stats->bytes / seconds / (1LU << 20));
  info("...total items forwarded from ongoing put() ops {}",
       stats->forwarded_items);
  info("...total bytes forwarded from ongoing put() ops {}",
       print_iec_bytes(stats->forwarded_bytes, true));

  return {};
}

DBStatus<detail::SnapshotStats>
Snapshotter::snapshot_one(UniqueLoglet srcll)
{
  Loglet &src = *srcll;

  SnapshotStats stats{};
  stats.bytes = src.size();

  // write out items in this loglet.
  for (ItemHeader &h : src) {
    stats.items++;
    ItemHeader *src_item = std::addressof(h);

    // snapshotting of the loglet is finished if every live item has
    // been copied into buffers destined for the snapshot file.
    if (src.live_size() == stats.live_bytes)
      break;

    // optimization: skip dead items that can be detected without locking.
    auto storage = std::atomic_ref<ItemStorage>(src_item->storage);
    if (storage.load(std::memory_order_acquire) == ItemStorage::kTombstone)
      continue;

    // only include items in the lsn range of the snapshot.
    // Note that DB if sync() returns an lsn, all items covered by that
    // sync() call have lsns *less than* the returned lsn.
    // Which is end_lsn_ is non-inclusive.
    if (src_item->lsn < begin_lsn_ || src_item->lsn >= end_lsn_)
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
    if (!ret) [[unlikely]]
      return ret.error();
    assert(found ||
           storage.load(std::memory_order_relaxed) == ItemStorage::kTombstone);
  }

  return {stats};
}

void
Snapshotter::run(ItemAllocator &item_allocator)
{
  DBStatus<void> ret = snapshot();
  if (!ret.has_value()) {
    warn("snapshot failed");
    promise_.set_value(ret);
    return;
  }

  ElapsedTimer snap_sync_timer{};
  ret = sync();
  if (!ret.has_value()) {
    warn("snapshot failed");
    promise_.set_value(ret);
  }

  debug("syncing snapshot file to storage took {:.3f} s",
        snap_sync_timer.elapsed());

  promise_.set_value({});
}

} // namespace hogd
