#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "storage_object.hh"

namespace hogd {

class StorageDeltaClassChecker {
public:
  StorageDeltaClassChecker(std::optional<StorageObject> const &base_object);
  StorageDeltaClassChecker(const StorageDeltaClassChecker &) = delete;

  StorageReserveResult try_set(StorageDeltaClass const &new_obj);
  void revert_one();
  void commit_one();

  // returns the committed StorageDeltaClass from the current round, if one
  // exists
  std::optional<StorageDeltaClass> commit_round();
  void rewind_round();

  // called when deletion happens in enclosing revertableobject in a round that
  // commits
  void clear_required_type();

private:
  // There are fancier ways of doing this.
  // The paper implementation has these, as well as groundhog/junkd.
  // Ignoring those for now, for simplicity and to make this review faster.

  // Optimiziation: If a delta commits, this value is set to sdc_.get()
  // and then the rest of the commit/rewind operations become read-only
  std::atomic<const StorageDeltaClass *> finalized_delta_class_ = nullptr;
  std::optional<StorageObjectType> required_type_;

  std::mutex mtx_;
  std::unique_ptr<const StorageDeltaClass> sdc_;
  uint32_t tag_ = 0;
};

} // namespace hogd
