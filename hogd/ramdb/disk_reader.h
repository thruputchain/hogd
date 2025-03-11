// disk_reader.h - the read path for persistent on-disk storage.
//
// In order to unlock some concurrency, ramdb uses a pipelined approach with
// two threads. The first thread reads, parses, and checksums item changes from
// the on-disk logs. The second thread then inserts the item changes into the
// in-memory map.

#pragma once

#include <optional>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/ramdb/io.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

class DiskReader {
public:
  DiskReader(LogletAllocator &allocator, ScalableMap &map)
    : allocator_(allocator), map_(map)
  {}
  ~DiskReader() = default;

  // Disable move and copy.
  DiskReader(DiskReader &&r) = delete;
  DiskReader &operator=(DiskReader &&r) = delete;
  DiskReader(const DiskReader &r) = delete;
  DiskReader &operator=(const DiskReader &r) = delete;

  // load loads a database from disk. It loads up to lsn if specified, or
  // otherwise the highest valid LSN. It returns the LSN that was loaded if
  // successful.
  DBStatus<uint64_t> load(Directory &dir,
                          std::optional<uint64_t> lsn = std::nullopt);

  // verify checks the integrity of a database on disk. it returns the highest
  // LSN that is valid.
  DBStatus<uint64_t> verify(Directory &dir);

private:
  // the allocator to use for loglet buffers.
  LogletAllocator &allocator_;
  // the map to insert items into.
  ScalableMap &map_;

  // load_log_file calls func with a buffer of changes for each committed LSN in
  // just one log file.
  DBStatus<void> load_log_file(
      File &f, uint64_t &cur_lsn, std::optional<uint64_t> target_lsn,
      const std::function<DBStatus<void>(uint64_t lsn,
                                         std::vector<UniqueLoglet> &&)> &func);

  // load_database calls func with a buffer of changes for each committed LSN
  // in an entire database.  It extracts these changes from the log files in
  // dir. It stops when target_lsn is reached or there is a failure.
  // Regardless, it sets loaded_lsn to the last LSN that was loaded
  // successfully.
  DBStatus<void> load_database(
      Directory &dir,
      const std::function<DBStatus<void>(uint64_t lsn,
                                         std::vector<UniqueLoglet> &&)> &func,
      std::optional<uint64_t> target_lsn, uint64_t *loaded_lsn = nullptr);

  // apply_changes loads a buffer of changes into map_.
  DBStatus<void> apply_changes(uint64_t lsn, std::vector<UniqueLoglet> &&log);
};

} // namespace hogd
