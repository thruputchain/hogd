#pragma once

#include <vector>

#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

namespace detail {

// SnapshotStats stores statistics about snapshotting.
struct SnapshotStats {
  size_t items;
  size_t live_items;
  size_t bytes;
  size_t live_bytes;
  size_t forwarded_items;
  size_t forwarded_bytes;

  SnapshotStats &operator+=(const SnapshotStats &stats)
  {
    items += stats.items;
    live_items += stats.live_items;
    bytes += stats.bytes;
    live_bytes += stats.live_bytes;
    forwarded_items += stats.forwarded_items;
    forwarded_bytes += stats.forwarded_bytes;
    return *this;
  }
};

} // namespace detail

class Snapshotter {
public:
  Snapshotter(WorkerPool &pool, ScalableMap &map, ItemAllocator &item_allocator,
              uint64_t begin_lsn, uint64_t end_lsn,
              LogletAllocator &loglet_allocator, Directory &dir,
              std::string path, size_t max_file_size, int write_head_order)

    : pool_{pool}, map_{map}, item_allocator_{item_allocator},
      lls_{item_allocator.take_loglets()}, begin_lsn_{begin_lsn},
      end_lsn_{end_lsn}, disk_writer_{loglet_allocator, dir, std::move(path),
                                      max_file_size, write_head_order}
  {
    disk_writer_.init(begin_lsn); // start snap writer bg thread
    // Publish snapshot lsn so main threads are aware of the ongoing snapshot.
    map_.register_in_progress_snapshot(
        begin_lsn_, end_lsn_, [this](uint64_t hash, ItemHeader *src_item) {
          // If a put() operation runs into an item that is about to be
          // overwritten that should be part of the snapshot, it triggers
          // this callback to stuff a copy of the item in the snapshot's
          // disk-bound loglets.

          ScopedDiskWrite record = disk_writer_.write_record(hash);
          DBStatus<uint64_t> ret = record.modify(src_item);
          if (!ret) [[unlikely]]
            return;
          // ret LSN ignored; it is meaningless outside of
          // redo log.

          record.reset(); // trigger writeback

          perthread_stats_->forwarded_items++;
          perthread_stats_->forwarded_bytes += src_item->total_bytes();
        });
    info("snapshot in lsn range [{}, {}) will include {} loglets of items",
         begin_lsn_, end_lsn_, lls_.size());
  }

  std::future<DBStatus<void>> get_future() { return promise_.get_future(); }

  bool snapshot();

private:
  WorkerPool &pool_;
  ScalableMap &map_;
  ItemAllocator &item_allocator_;
  std::vector<UniqueLoglet> lls_;

  uint64_t begin_lsn_;
  uint64_t end_lsn_;

  DiskWriter disk_writer_;

  std::promise<DBStatus<void>> promise_;

  using SnapshotStats = detail::SnapshotStats;
  PerThread<SnapshotStats> perthread_stats_;

  std::optional<ElapsedTimer> timer_;
  using Completions = std::vector<std::future<DBStatus<SnapshotStats>>>;
  Completions completions_;
  DBStatus<detail::SnapshotStats> aggregated_stats_;
  std::optional<Completions::iterator> completions_it_;

  void start();
  bool check_completions();
  void finish();

  DBStatus<SnapshotStats> snapshot_one(UniqueLoglet src);

  std::vector<UniqueLoglet> &&take_loglets() { return std::move(lls_); }

  DBStatus<void> sync()
  {
    DBStatus<uint64_t> ret = disk_writer_.sync();
    if (!ret) [[unlikely]] {
      return ret.error();
    }
    return {};
  }
};

} // namespace hogd
