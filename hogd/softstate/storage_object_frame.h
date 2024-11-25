#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "hogd/softstate/uint128_reservation.h"
#include "storage_object.hh"
#include "x23/mvar.h"

namespace hogd {

// Something like the ReplayCache or a BoundedHashset will require
// internal data structures more complex than a StorageObject (which is really a
// serialization format more than an efficient data structure). The internals
// may need reimplementation when we have a database to interact with. However,
// the external interface and logic will look approximately like this.
class StorageObjectFrame {
public:
  StorageObjectFrame(std::optional<StorageObject> const &snapshot_state);
  StorageObjectFrame(const StorageObjectFrame &) = delete;

  // Required usage pattern: A transaction calls try_reserve_delta() on a delta,
  // and then either calls commit or revert on the delta.
  //
  // TODO(geoff): We can probably just remove the commit_delta() method
  // entirely.  Will wait to remove until we
  // have all the other StorageDelta types worked out.
  [[nodiscard]] StorageReserveResult
  try_reserve_delta(StorageDelta const &delta);
  void commit_delta(StorageDelta const &delta);
  void revert_delta(StorageDelta const &delta);

  std::optional<StorageObject> const &get_committed_object() const;

  void commit_round();
  void revert_round();

private:
  struct BytestringArgs {
    const Bytestring bytestring_set_;

    bool can_accept(Bytestring const &set_arg) const
    {
      return bytestring_set_ == set_arg;
    }

    StorageObject to_object() const
    {
      StorageObject object;
      object.type(StorageObjectType::kBytestring);
      object.bytestring() = bytestring_set_;
      return object;
    }
  };

  struct NNInt64SetAddArgs {
    const int64_t set_value_;
    int64_t total_subtracted_ = 0;
    UInt128Reservation total_added_;

    bool can_accept(int64_t const &set_arg) const;
    StorageObject to_object() const;
  };

  using variant_t = std::variant<BytestringArgs, NNInt64SetAddArgs>;

  struct RefcountedVariant {
    variant_t variant_;
    uint32_t refcount_ = 0;
  };

  std::optional<StorageObject> committed_base_object_;

  x23::MVar<RefcountedVariant> variant_;

  std::atomic<bool> deleted_last_;

  StorageReserveResult
  try_reserve_bytestring_set(Bytestring const &bytestring_set);
  StorageReserveResult try_reserve_nnint64_set_add(int64_t set_value,
                                                   int64_t add_value);

  void revert_bytestring_set();
  void revert_nnint64_set_add(int64_t add_value);

  bool type_guard(StorageObjectType expected_type) const;

  void clear_modifications();

  static void release_variant(RefcountedVariant &variant);

  template<typename VariantArm>
  static VariantArm &get_variant_arm_unchecked(RefcountedVariant &variant)
  {
    return std::get<VariantArm>(variant.variant_);
  }

  template<typename VariantArm, typename... VariantArgs>
  static VariantArm *try_set_variant(RefcountedVariant &variant,
                                     VariantArgs const &...args);
};

} // namespace hogd
