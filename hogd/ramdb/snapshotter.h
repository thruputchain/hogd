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
  Snapshotter(WorkerPool &pool, DiskWriter &disk_writer, ScalableMap &map,
              std::vector<UniqueLoglet> &&all_lls, uint64_t begin_lsn,
              uint64_t end_lsn)
    : pool_{pool}, disk_writer_{disk_writer}, map_{map},
      lls_{std::move(all_lls)}, begin_lsn_{begin_lsn}, end_lsn_{end_lsn},
      future_{promise_.get_future()}
  {
    info("snapshot in lsn range [{}, {}) will include {} loglets of items",
         begin_lsn_, end_lsn_, lls_.size());
  }

  void run(ItemAllocator &item_allocator);

  DBStatus<void> get_result() { return future_.get(); }

private:
  DBStatus<void> snapshot();

  std::vector<UniqueLoglet> &&take_loglets() { return std::move(lls_); }

  DBStatus<void> sync()
  {
    DBStatus<uint64_t> ret = disk_writer_.sync();
    if (!ret) [[unlikely]] {
      return ret.error();
    }
    return {};
  }

  WorkerPool &pool_;
  DiskWriter &disk_writer_;
  ScalableMap &map_;
  std::vector<UniqueLoglet> lls_;

  uint64_t begin_lsn_;
  uint64_t end_lsn_;

  std::promise<DBStatus<void>> promise_;
  std::future<DBStatus<void>> future_;

  using SnapshotStats = detail::SnapshotStats;
  DBStatus<SnapshotStats> snapshot_one(Loglet &src);
};

} // namespace hogd
