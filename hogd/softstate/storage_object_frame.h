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

  // Required usage pattern: A transaction calls try_reserve_delta() on a delta,
  // and then either calls commit or revert on the delta.
  //
  // TODO(geoff): We can probably just remove the commit_delta() method
  // entirely. Finalizing a delta might slightly improve performance (removes
  // one cache line write) on subsequent deltas. Will wait to remove until we
  // have all the other StorageDelta types worked out.
  [[nodiscard]] StorageReserveResult
  try_reserve_delta(StorageDelta const &delta);
  void commit_delta(StorageDelta const &delta);
  void revert_delta(StorageDelta const &delta);

  std::optional<StorageObject> const &get_committed_object() const;

  // Assumes some synchronization barrier between try/commit_delta and
  // commit_round.
  //
  // Round_number is any monotonically increasing counter.  It
  // should increase after every successful AND failed block, in order to
  // garbage collect internal metadata. (although it would not be incorrect to
  // leave it as the block number -- failed blocks would just not be garbage
  // collected until a successful block is found).
  void commit_round(uint64_t round_number);
  void revert_round(uint64_t round_number);

  ~StorageObjectFrame();

private:
  std::optional<StorageObject> committed_base_object_;

  std::atomic<uint64_t> delta_tag_and_ptr_ = 0;

  std::atomic<bool> deleted_last_;

  StorageReserveResult
  try_reserve_bytestring_set(Bytestring const &bytestring_set);
  StorageReserveResult try_reserve_nnint64_set_add(int64_t set_value,
                                                   int64_t add_value);

  void revert_bytestring_set();
  void revert_nnint64_set_add(int64_t add_value);

  bool type_guard(StorageObjectType expected_type) const;

  void clear_modifications();
};

} // namespace hogd
