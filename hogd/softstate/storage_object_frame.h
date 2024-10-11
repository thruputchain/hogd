#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "hogd/softstate/delta_class_checker.h"
#include "storage_object.hh"

namespace hogd {

class StorageObjectFrame {
public:
  StorageObjectFrame(std::optional<StorageObject> const &snapshot_state);
  StorageObjectFrame(const StorageObjectFrame &) = delete;

  StorageReserveResult __attribute__((warn_unused_result))
  try_reserve_delta(StorageDelta const &delta);
  void commit_delta(StorageDelta const &delta);
  void revert_delta(StorageDelta const &delta);

  std::optional<StorageObject> const &get_committed_object() const;

  void commit_round();
  void revert_round();

private:
  std::optional<StorageObject> committed_base_object_;

  StorageDeltaClassChecker delta_class_checker_;

  std::atomic<bool> deleted_last_;

  std::atomic<int64_t> nnint64_total_subtracted = 0;
  std::atomic<uint64_t> nnint64_total_added = 0;
  std::atomic<bool> nnint64_add_overflow_flag = false;

  void clear_modifications();
};

} // namespace hogd
