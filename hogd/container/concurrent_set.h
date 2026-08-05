#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "x23/x23/nmvec.h"

namespace hogd {

// This should require std::atomic<T>::is_always_lock_free but g++ gives back
// false for uint128_t, which is our main use case.
template<typename T>
concept concurrent_set_element =
    std::unsigned_integral<T> && requires(const T &a) {
      { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
    };

// Careful here. While these seem to be tunable, kEmpty must match whatever the
// default constructor of T does since that is what we use to initialize the
// hash set arrays. kInserting marks a slot that an insert has claimed but not
// yet published; see ConcurrentSet::try_insert.
template<typename T> struct DefaultConcurrentSetTraits {
  static constexpr T kEmpty = T{};
  static constexpr T kTombstone = ~T{};
  static constexpr T kInserting = ~T{} - T{1};
  static size_t hash(const T &v) { return std::hash<T>{}(v); }
};

// Why an insert did not add the value. A caller feeding a set untrusted values
// needs to tell these apart without knowing how the set represents empty and
// erased slots: kValueReserved says the set can never store this value, which
// is a property of the input rather than of the set's contents.
enum class InsertResult {
  kInserted,       // this call added the value
  kAlreadyPresent, // the value was already in the set
  kValueReserved,  // the set reserves this value to mark slot state
};

template<concurrent_set_element T,
         typename TraitsT = DefaultConcurrentSetTraits<T>>
class alignas(alignof(T)) ConcurrentSet {
public:
  using Traits = TraitsT;

  static_assert(Traits::kEmpty != Traits::kTombstone &&
                    Traits::kEmpty != Traits::kInserting &&
                    Traits::kTombstone != Traits::kInserting,
                "The three reserved slot markers must be distinct");

  ConcurrentSet() : initial_level_{Traits::kEmpty}, capacity_{kInitialCapacity}
  {}

  ConcurrentSet(const ConcurrentSet &other) = delete;
  ConcurrentSet &operator=(const ConcurrentSet &other) = delete;

  // Return the number of live elements in the set. Since inserts
  // and erases are tracked separately and the count fields are not
  // the linearization point, the returned value isn't precise under
  // concurrent inserts and erases.
  size_t size() const
  {
    return inserts_.load(std::memory_order_relaxed) -
           erases_.load(std::memory_order_relaxed);
  }

  InsertResult try_insert(const T &value)
  {
    if (is_reserved(value)) {
      return InsertResult::kValueReserved;
    }

    size_t hash = Traits::hash(value);

    size_t level = -1;
    while (true) {
    outer:
      ++level;
      size_t level_capacity = entries_for_level(level);
      assert((level_capacity & (level_capacity - 1)) == 0);

      for (size_t i = 0; i < level_capacity; i++) {
        size_t index = (hash + i) & (level_capacity - 1);
        auto *l = get_level(level);

        T v = l[index].load(std::memory_order_acquire);
        if (v == Traits::kInserting) {
          v = cancel_claim(l[index]);
        }
        if (v == value) {
          return InsertResult::kAlreadyPresent;
        }
        if (v != Traits::kEmpty) {
          continue;
        }
        // if we found an empty slot, we can only insert to it if it is the
        // deepest level, otherwise we need to move down to the next level.
        if (level < levels() - 1) {
          // Re-read the slot. Between our original load (which saw kEmpty) and
          // the levels() check above, a concurrent thread may have CAS'd our
          // value into this slot and then triggered try_resize(), adding a
          // deeper level. Without this re-read, we would skip to the new
          // deeper level without ever seeing the concurrent insert, and could
          // insert a duplicate there. The remaining TOCTOU (value inserted
          // after this re-read) is covered by the stale-level check below.
          v = l[index].load(std::memory_order_acquire);
          if (v == Traits::kInserting) {
            v = cancel_claim(l[index]);
          }
          if (v == value) {
            return InsertResult::kAlreadyPresent;
          }
          if (v != Traits::kEmpty) {
            continue;
          }
          goto outer;
        }

        // We are at the deepest level, so claim the slot. A claim reserves the
        // slot without publishing a value. erase() never matches kInserting, so
        // an erase can no longer consume its tombstone on an insert that has
        // not committed to publishing yet, which is what let a stale copy
        // survive in a level that a resize had displaced.
        if (l[index].compare_exchange_strong(v, Traits::kInserting,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
          // Count the claim, not the publication: the slot is consumed either
          // way, and try_resize() derives the load factor from inserts_.
          inserts_.fetch_add(1, std::memory_order_relaxed);

          // A resize may have added a deeper level between our levels() check
          // and the claim. If so, a concurrent inserter of the same value could
          // have placed it in the new level, so give up this claim instead of
          // publishing a second copy, and retry from level 0.
          if (level < levels() - 1) {
            cancel_claim(l[index]);
            level = static_cast<size_t>(-1);
            goto outer;
          }

          T claimed = Traits::kInserting;
          if (!l[index].compare_exchange_strong(claimed, value,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
            // A concurrent insert cancelled our claim, so this slot is now a
            // tombstone and that thread already accounted for the erase. Start
            // over: the value may have been published in another slot since.
            level = static_cast<size_t>(-1);
            goto outer;
          }

          // It might have been the case that there was sufficient capacity when
          // we started try_insert, but we don't know that our insert didn't
          // raise the load further. So, resize if necessary before retrying.
          try_resize();
          return InsertResult::kInserted;
        }
        // Another thread took the slot first.
        if (v == Traits::kInserting) {
          v = cancel_claim(l[index]);
        }
        if (v == value) {
          return InsertResult::kAlreadyPresent;
        }
      }

      // this should *almost* never happen. the one place we can run into this
      // is if lots of concurrent inserts start but don't complete and there
      // isn't sufficient space in the level they started in for them all to
      // complete. If this does happen, first check to see if any levels have
      // been added, and restart there. Otherwise, try to add a level.
      if (level < levels() - 1) {
        goto outer;
      }
      else {
        try_resize();
        --level; // Retry this level: ++level at outer restores it.
                 // Concurrent inserts will eventually update inserts_,
                 // allowing try_resize to add a level, or free up slots.
      }
    }
  }

  bool contains(T value) { return do_contains(value, false); }
  bool erase(T value) { return do_contains(value, true); }

  // iterator over all of the non-erased values
  class Iterator {
    ConcurrentSet *set_;
    size_t index_;
    size_t level_;

  public:
    Iterator(ConcurrentSet *set, size_t index, size_t level)
      : set_(set), index_(index), level_(level)
    {}
    T operator*() const
    {
      return set_->get_level(level_)[index_].load(std::memory_order_acquire);
    }

    Iterator &operator++()
    {
      do {
        index_++;
        if (index_ == entries_for_level(level_)) {
          level_++;
          index_ = 0;
        }
      } while (*this != set_->end() && is_reserved(**this));
      return *this;
    }

    bool operator==(const Iterator &other) const
    {
      return index_ == other.index_ && level_ == other.level_;
    }
    bool operator!=(const Iterator &other) const { return !(*this == other); }
  };

  Iterator begin()
  {
    auto it = Iterator(this, 0, 0);
    if (is_reserved(*it)) {
      ++it;
    }
    return it;
  }

  Iterator end() { return Iterator(this, 0, levels()); }

private:
  // The values the set reserves to mark slot state, which it therefore cannot
  // store. Kept internal: callers learn about them from try_insert's result
  // rather than by asking, so the set's slot encoding stays its own business.
  static bool is_reserved(const T &v)
  {
    return v == Traits::kEmpty || v == Traits::kTombstone ||
           v == Traits::kInserting;
  }

  // Tombstone a slot that holds an in-flight claim. The claim's owner calls
  // this to give up a claim it can no longer publish, and another insert calls
  // it to cancel a claim it must not walk past. Returns what the slot holds
  // afterwards: kTombstone if this call or a racing one cancelled the claim, or
  // the value the owner published just before we got here. No thread ever waits
  // on a claim, so try_insert keeps the progress guarantee it had before: a
  // cancelled owner retries, and the thread that cancelled it moves on.
  T cancel_claim(std::atomic<T> &slot)
  {
    T expected = Traits::kInserting;
    if (slot.compare_exchange_strong(expected, Traits::kTombstone,
                                     std::memory_order_release,
                                     std::memory_order_relaxed)) {
      erases_.fetch_add(1, std::memory_order_relaxed);
      return Traits::kTombstone;
    }
    return expected;
  }

  constexpr static size_t entries_for_level(size_t level)
  {
    return kInitialCapacity * (1 << level);
  }

  std::atomic<T> *get_level(size_t level)
  {
    if (level == 0) {
      return initial_level_;
    }
    return extra_levels_[level - 1].get();
  }

  size_t levels() const { return extra_levels_.size() + 1; }

  // If the number of inserts hasn't exceeded the load factor of the set,
  // do nothing, otherwise, add a new level to the set.
  void try_resize()
  {
    size_t capacity;
    while ((capacity = capacity_.load(std::memory_order_acquire)) ==
           kCapacityResizeLock) {
      cpu_relax();
    }

    size_t inserts = inserts_.load(std::memory_order_relaxed);
    if (inserts < capacity * kResizeLoadFactor) {
      // resize not needed.
      return;
    }

    size_t cap = capacity;
    while (cap != kCapacityResizeLock) {
      if (capacity_.compare_exchange_strong(cap, kCapacityResizeLock)) {
        // we are the ones who are going to resize, but first check to make sure
        // someone else didn't already acquire/release the resize lock to do the
        // resize.
        size_t inserts = inserts_.load(std::memory_order_relaxed);
        if (inserts < cap * kResizeLoadFactor) {
          // resize not needed, release and return.
          capacity_.store(cap, std::memory_order_release);
          return;
        }

        size_t n_levels = levels();
        size_t added_cap = entries_for_level(n_levels);
        extra_levels_.emplace_back(
            std::make_unique<std::atomic<T>[]>(added_cap));
        capacity_.store(cap + added_cap, std::memory_order_release);
        break;
      }
    }

    while (capacity_.load(std::memory_order_acquire) == kCapacityResizeLock) {
      cpu_relax();
    }
  }

  // Search for a value in the set. If erase is true, erase it.
  // Returns true if the value was found (and erased if erase is true).
  bool do_contains(T value, bool erase)
  {
    // A reserved value can never have been stored, so it is definitively
    // absent and there is nothing to erase. Answering instead of panicking is
    // what keeps callers from having to pre-screen untrusted values.
    if (is_reserved(value)) {
      return false;
    }

    size_t hash = Traits::hash(value);

    size_t level = -1;
    while (true) {
    outer:
      size_t level_capacity = entries_for_level(++level);
      assert((level_capacity & (level_capacity - 1)) == 0);

      for (size_t i = 0; i < level_capacity; i++) {
        size_t index = (hash + i) & (level_capacity - 1);
        auto *l = get_level(level);

        T v = l[index].load(std::memory_order_acquire);
        if (v == value) {
          bool r = true;
          if (erase) {
            // This is subtle. Note here the only overwrites allowed on an entry
            // are for ones that are non-empty and non-tombstone. Hence, if we
            // see CAS failure, it must be because another thread erased the
            // entry, so there is no need to retry.
            r = l[index].compare_exchange_strong(v, Traits::kTombstone,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
            if (r) {
              erases_.fetch_add(1, std::memory_order_relaxed);
            }
          }
          return r;
        }
        // Anything else keeps the probe going, including a slot claimed by an
        // in-flight insert. Unlike try_insert we neither wait on nor cancel a
        // claim: cancelling one would destroy an insert of some unrelated
        // value, and if the claim does settle into value then that insert is
        // concurrent with this call, so ordering it after us is a valid
        // linearization. Walking past a claim cannot hide an already-published
        // copy, since the probe continues to the end of the sequence.
        if (v != Traits::kEmpty) {
          continue;
        }

        if (level < levels() - 1) {
          goto outer;
        }

        return false;
      }

      // in case a level was added since we started looking for a value in the
      // current level, restart the search in the new level.
      if (level < levels() - 1) {
        goto outer;
      }

      return false;
    }

    // Shouldn't happen: the hash table should always resize before there
    // are no empty slots left in it.
    panic("ConcurrentSet::contains should never get here");
  }

  static constexpr size_t kCapacityResizeLock = ~0;

  // Initial capacity of the hash table. Must be a power of two.
  static constexpr size_t kInitialCapacity = 8;
  static_assert((kInitialCapacity & (kInitialCapacity - 1)) == 0,
                "Initial capacity must be a power of two");
  // This container does not rehash when it grows, so needs to resize well
  // before it runs out of space. Numbers much above 0.5 cause large sets
  // to get very slow.
  static constexpr double kResizeLoadFactor = 0.5;

  std::atomic<T> initial_level_[kInitialCapacity];
  // Each level is an array of entries_for_level(level) entries. Once more than
  // kResizeLoadFactor of the entries are filled for the whole structure, a new
  // level is added which is twice as large as the previous level.
  x23::NMVec<std::unique_ptr<std::atomic<T>[]>> extra_levels_;

  // Number of insertions over all time to the structure. Used to give back
  // approximate size for the structure and to decide on resizing.
  std::atomic<size_t> inserts_{0};
  // Number of erasures over all time to the structure. Used in size() to
  // account for erasures.
  std::atomic<size_t> erases_{0};

  // Amount of total space in all levels of the hash table.
  std::atomic<size_t> capacity_{kInitialCapacity};

  friend class ConcurrentSetHelper;
};

} // namespace hogd
