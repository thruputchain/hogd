#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <set>

#include "hogd/softstate/uint128_reservation.h"
#include "storage_object.hh"
#include "x23/mvar.h"

namespace hogd {

// Something like the ReplayCache or a BoundedHashset will require
// internal data structures more complex than a StorageObject (which is really a
// serialization format more than an efficient data structure). The internals
// may need reimplementation when we have a database to interact with. However,
// the external interface should look approximately like this.
class StorageObjectFrame {
public:
  StorageObjectFrame(std::optional<StorageObject> const &snapshot_state);

  StorageObjectFrame(const StorageObjectFrame &) = delete;

  // Required usage pattern: A transaction calls try_reserve_delta() on a delta,
  // and then either calls commit or revert on the delta.
  //
  // A lock can be acquired mid transaction, but try_reserve_delta() can only be
  // called exactly once per transaction. The lock, if supplied, is immediately
  // consumed by try_reserve_delta. Locks that are not consumed must be
  // explicitly released at the end of a (failed) transaction.
  // 
  // If lock.has_value(), then lock->lock_type != kNone.
  //
  // Each transaction should call try_reserve_delta() at most ONCE per object.
  // This means I need to condense things into one StorageDelta object
  // (or I could swap for a list, but that's annoying).
  //
  // TODO(geoff): We can probably just remove the commit_delta() method
  // entirely.  Will wait to remove until we
  // have all the other StorageDelta types worked out.
  [[nodiscard]] StorageReserveResult
  try_reserve_delta(StorageDelta const &delta,
                    std::optional<StorageLock> &lock);

  // Always returns successfully when downgrading lock type
  [[nodiscard]] StorageReserveResult
  try_change_lock(StorageLockChange const &lock_change);

  void revert_delta(StorageDelta const &delta);
  void commit_delta(StorageDelta const &delta);

  std::optional<StorageObject> const &get_committed_object() const;
  std::optional<StrippedStorageObject> get_stripped_committed_object() const;

  void commit_round();
  void revert_round();

private:
  struct BytestringArgs {
    const Bytestring bytestring_set_;

    bool can_accept(Bytestring const &candidate_bytestring) const
    {
      return candidate_bytestring == bytestring_set_;
    }

    std::optional<StorageObject>
    to_object(std::optional<StorageObject> const & /*unused*/) const
    {
      StorageObject object;
      object.type(StorageObjectType::kBytestring);
      object.bytestring() = bytestring_set_;
      return object;
    }
  };

  struct NNInt64SetAddArgs {
    const int64_t set_value_;
    // total subtracted by reservations AND locks
    int64_t total_subtracted_ = 0;

    UInt128Reservation total_added_;

    bool can_accept(int64_t candidate_set_value) const
    {
      return candidate_set_value == set_value_;
    }

    std::optional<StorageObject>
    to_object(std::optional<StorageObject> const &) const;
  };

  struct ReplayCacheArgs {
    std::set<uint64_t> new_nonces_;
    std::optional<uint64_t> new_max_clear_threshold_;

    static bool can_accept() { return true; }

    std::optional<StorageObject>
    to_object(std::optional<StorageObject> const &previous_object) const;
  };

  struct MessageQueueArgs {
    uint32_t num_messages_consumed_ = 0;

    std::set<MessageQueueEvent> new_events_;

    static bool can_accept() { return true; }

    std::optional<StorageObject>
    to_object(std::optional<StorageObject> const &previous_object) const;
  };

  struct DeleteArgs {

    static bool can_accept() { return true; }

    static std::optional<StorageObject>
    to_object(std::optional<StorageObject> const & /* unused */)
    {
      return std::nullopt;
    }
  };

  struct ExclusiveLockedArgs {

    static std::optional<StorageObject>
    to_object(std::optional<StorageObject> const & /* unused */)
    {
      throw std::runtime_error("impossible");
    }
  };

  using variant_t =
      std::variant<BytestringArgs, NNInt64SetAddArgs, ReplayCacheArgs,
                   MessageQueueArgs, DeleteArgs, ExclusiveLockedArgs>;

  struct RefcountedVariant {
    variant_t variant_;
    uint32_t refcount_ = 0;

    void set_typeclass_from_delta(StorageDelta const &delta);
    void set_exclusive_locked();
  };

  std::optional<StorageObject> committed_base_object_;

  x23::MVar<RefcountedVariant> variant_;

  // bytestring
  static StorageReserveResult
  try_reserve_bytestring_set(RefcountedVariant &v,
                             Bytestring const &bytestring_set,
                             std::optional<StorageLock> const& lock);

  // nnint64
  static StorageReserveResult 
  try_reserve_nnint64_set_add(RefcountedVariant& v, int64_t set_value,
                                                   int64_t add_value,
                                                   std::optional<StorageLock>& lock);
  static void revert_nnint64_set_add(RefcountedVariant &v, int64_t add_value);

  /*

  // replay cache
  StorageReserveResult
  try_reserve_replay_cache_inserts(x23::Vec<uint64_t> const &nonces);
  void revert_replay_cache_inserts(x23::Vec<uint64_t> const &nonces);
  StorageReserveResult try_reserve_replay_cache_clear(uint64_t threshold);
  void commit_replay_cache_clear(uint64_t threshold);
  void revert_replay_cache_clear();

  // message queue
  StorageReserveResult try_reserve_mq_append(MessageQueueEvent const &event);
  void revert_mq_append(MessageQueueEvent const &event);
  StorageReserveResult try_reserve_mq_consume(uint32_t num_messages);
  void revert_mq_consume();

  */

  void clear_modifications();

  static void acquire_variant_reference(RefcountedVariant &variant)
  {
    variant.refcount_++;
  }
  static void release_variant_reference(RefcountedVariant &variant)
  {
    assert(variant.refcount_ > 0);
    variant.refcount_--;
  }

  template<typename VariantArm>
  static VariantArm &get_variant_arm_unchecked(RefcountedVariant &variant)
  {
    return std::get<VariantArm>(variant.variant_);
  }

  template<typename VariantArm, typename... VariantArgs>
  static VariantArm *try_set_variant(RefcountedVariant &variant,
                                     VariantArgs const &...args);

  template<typename VariantArm, typename... VariantArgs>
  static void set_variant_from_exclusive_lock(RefcountedVariant &variant,
                                              VariantArgs const &...args);
};

} // namespace hogd
