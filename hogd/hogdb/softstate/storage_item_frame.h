#pragma once

#include <cassert>
#include <exception>
#include <optional>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/hogdb/softstate/uint256_reservation.h"
#include "storage_item.hh"

namespace hogd {

// Manage concurrent accesses to a StorageItem in the database.
//
// This object is not threadsafe.  It should only be accessed while
// some external lock on it is held.
//
// TODO(geoff): Some data types will require internal data structures more
// complex than a StorageItem,(which is really a serialization format more than
// an efficient data structure). The internals may need reimplementation when we
// have a database to interact with. However, the external interface should look
// approximately like this.
class StorageItemFrame {
public:
  StorageItemFrame(std::optional<StorageItem> const &snapshot_state);

  // Disable move and copy
  StorageItemFrame(const StorageItemFrame &) = delete;
  StorageItemFrame &operator=(StorageItemFrame const &) = delete;
  StorageItemFrame(StorageItemFrame &&) = delete;
  StorageItemFrame &operator=(StorageItemFrame &&) = delete;

  // Required usage pattern: A transaction calls try_reserve_delta() on a delta,
  // and then either calls commit or revert on the delta
  //
  // Each transaction should call try_reserve_delta() at most ONCE per object.
  // The executor should therefore condense all modifications into one
  // StorageDelta per each modified item.
  //
  // Returns true iff reservation is successful.
  // No-op if the operation fails.
  [[nodiscard]] bool try_reserve_delta(StorageDelta const &delta);

  void revert_delta(StorageDelta const &delta);
  void commit_delta(StorageDelta const &delta);

  std::optional<StorageItem> const &get_snapshot_item() const;

  // All pending transactions should finish before commit_round is called.
  // That is, every try_reserve_delta() is followed by a revert_delta or a
  // commit_delta.
  void commit_round();
  void revert_round();

private:
  // Each of the following types represents one possible state that the item
  // may be in, given any pending reservations. Each has a typeclass_match()
  // method, which returns whether the variant can accept a delta of that
  // typeclass.
  //
  // Each also has a to_item method(), which should be called
  // after a block commits(during commit_block()). This converts the
  // committed deltas to a StorageItem for the next block snapshot. All
  // transactions should either revert or commit before to_item() is called.

  // Bytestring state
  struct BytestringArgs {
    const Bytestring bytestring_set_;

    bool typeclass_match(Bytestring const &candidate_bytestring) const
    {
      return candidate_bytestring == bytestring_set_;
    }
    std::optional<StorageItem> to_item() const;
  };

  // NNInt state
  struct NNIntSetAddArgs {
    const int128_t set_value_;
    // total subtracted by any pending or committed reservations
    int128_t total_subtracted_ = 0;
    UInt256Reservation total_added_;

    bool typeclass_match(int128_t candidate_set_value) const
    {
      return candidate_set_value == set_value_;
    }
    std::optional<StorageItem> to_item() const;
  };

  // Deleted state
  struct DeleteArgs {
    static bool typeclass_match() { return true; }
    static std::optional<StorageItem> to_item() { return std::nullopt; }
  };

  // No deltas have been applied or reserved.
  // If variant.refcount is 0, then the variant is in this state.
  struct UnsetState {
    static bool typeclass_match() { return true; };

    // to_item should never be called if refcount is 0.
    static std::optional<StorageItem> to_item() { std::terminate(); }
  };

  using variant_t =
      std::variant<BytestringArgs, NNIntSetAddArgs, DeleteArgs, UnsetState>;

  // Contents of the variant are ignored if refcount_ is 0.
  // Every successful try_reserve increases the refcount, and revert reduces it.
  class RefcountedVariant {
  public:
    variant_t variant_ = UnsetState{};
    uint32_t refcount_ = 0;

    void acquire_reference() { ++refcount_; }

    void release_reference()
    {
      assert(refcount_ > 0);
      --refcount_;
      if (refcount_ == 0)
        variant_ = UnsetState{};
    }

    void clear()
    {
      refcount_ = 0;
      variant_ = UnsetState{};
    }

    // If these methods return true, a reference has been acquired
    bool try_reserve_delete();
    bool try_reserve_bytestring_set(Bytestring const &bytestring);
    bool try_reserve_nnint_set_add(int128_t set_value, int128_t add_value);

    // does not release the reference
    void revert_nnint_set_add(int128_t add_value);

  private:
    // Does not manage adjusting the reference count
    template<typename VariantArm, typename... VariantArgs>
    VariantArm *try_set_variant(VariantArgs const &...args);

    template<typename VariantArm> VariantArm &get_variant_arm_unchecked()
    {
      return std::get<VariantArm>(variant_);
    }
  };

  void clear_modifications();

  std::optional<StorageItem> committed_base_item_;
  RefcountedVariant variant_;
};

} // namespace hogd
