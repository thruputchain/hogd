#pragma once

#include <cstdint>
#include <optional>
#include <set>

#include "hogd/hogdb/softstate/uint256_reservation.h"
#include "storage_item.hh"
#include "x23/mvar.h"

namespace hogd {

// Manage concurrent accesses to a StorageItem in the database.
//
// Some data types will require internal data structures more complex than a
// StorageItem,(which is really a serialization format more than an efficient
// data structure). The internals may need reimplementation when we have a
// database to interact with. However, the external interface should look
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
  // This means I need to condense things into one StorageDelta object
  // (or I could swap for a list, but that's annoying).
  //
  // Returns true if reservation is successful.
  //
  // TODO(geoff): We can probably just remove the commit_delta() method
  // entirely.  Will wait to remove until we
  // have all the other StorageDelta types worked out.
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

  using variant_t = std::variant<BytestringArgs, NNIntSetAddArgs, DeleteArgs>;

  // Contents of the variant are ignored if refcount_ is 0.
  // Every successful try_reserve increases the refcount, and revert reduces it.
  class RefcountedVariant {
  public:
    variant_t variant_;
    uint32_t refcount_ = 0;

    void acquire_reference() { ++refcount_; }

    void release_reference()
    {
      assert(refcount_ > 0);
      --refcount_;
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
  x23::MVar<RefcountedVariant> variant_;
};

} // namespace hogd
