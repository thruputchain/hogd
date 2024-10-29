#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "hogd/softstate/atomic_uint128.h"

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

  // Assumes some synchronization barrier between try/commit_delta and
  // commit_round Round_number is any monotonically increasing counter.  It
  // should increase after every successful AND failed block, in order to
  // garbage collect internal metadata. (although it would not be incorrect to
  // leave it as the block number -- failed blocks would just not be garbage
  // collected until a successful block is found).
  void commit_round(uint64_t round_number);
  void revert_round(uint64_t round_number);

private:
  std::optional<StorageObject> committed_base_object_;

  std::atomic<uint64_t> delta_tag_and_ptr_ = 0;

  std::atomic<bool> deleted_last_;

  StorageReserveResult
  try_reserve_bytestring_set(Bytestring const &bytestring_set);
  void revert_bytestring_set();

  StorageReserveResult try_reserve_nnint64_set_add(int64_t set_value,
                                                   int64_t add_value);
  void revert_nnint64_set_add(int64_t add_value);

  void clear_modifications();
};

} // namespace hogd
