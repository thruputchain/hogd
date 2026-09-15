#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace hogd {

// flat_key_set.h - an insert-only, open-addressed set of keys that carry
// their own hash.
//
// Built for SystemCallHandler::loaded_keys_, the per-transaction set that
// decides whether a storage load is the transaction's first touch of a key.
// std::unordered_set costs that probe two dependent cache misses -- the
// bucket, then the node -- and a node allocation on top. With tens of
// thousands of distinct keys in one transaction (the sweep in
// rv32_syscall_entry_bench goes to 262144), that was measured at 100-240 ns
// per key, the whole of the wall-clock cost #3906 added to a loading read.
//
// This stores the hashes in one contiguous array and the key bytes in another,
// both power-of-two sized, and probes linearly. A lookup reads the hash array
// first, which for 65536 keys is 1 MB and stays in L2; the key array is
// touched only on a hash match, so a miss (a true first touch) usually costs
// one L2 hit and a hit costs one cache miss. Measured against the std set on
// the same 76-byte keys, 4096 to 262144 of them: 2-3x cheaper on a hit, 4-6x
// on a miss, 2-4x on an insert.
//
// Membership is exact. The hash decides where to look, never whether two keys
// are equal: a probe is a hit only when the stored key compares equal, so a
// hash collision costs a compare, not an under-charge. That matters because
// the gas this set gates is consensus-visible.
//
// No erase and no iteration: the one use only grows within a transaction and
// dies with it, so there are no tombstones to reason about.
//
// Key requirements: `key.hash()` returning a size_t, and `key.bytes()`
// returning a `Key::Bytes const &` -- a std::array of the key's bytes, which
// is what equality is decided on. FrameItemKey is the intended key.
template<typename Key> class FlatKeySet {
public:
  FlatKeySet() = default;

  FlatKeySet(FlatKeySet &&other) noexcept = default;
  FlatKeySet &operator=(FlatKeySet &&other) noexcept = default;
  FlatKeySet(FlatKeySet const &) = delete;
  FlatKeySet &operator=(FlatKeySet const &) = delete;

  // True if [key] was not present and is now. Same contract as
  // std::unordered_set::insert(key).second.
  bool insert(Key const &key)
  {
    if ((size_ + 1) * 2 > capacity_) {
      grow();
    }
    const uint64_t tag = tag_of(key.hash());
    size_t i = slot_of(tag, mask_);
    for (;;) {
      if (tags_[i] == kEmpty) {
        tags_[i] = tag;
        payloads_[i] = key.bytes();
        ++size_;
        return true;
      }
      if (tags_[i] == tag && payloads_[i] == key.bytes()) {
        return false;
      }
      i = (i + 1) & mask_;
    }
  }

  bool contains(Key const &key) const
  {
    if (size_ == 0) {
      return false;
    }
    const uint64_t tag = tag_of(key.hash());
    size_t i = slot_of(tag, mask_);
    for (;;) {
      if (tags_[i] == kEmpty) {
        return false;
      }
      if (tags_[i] == tag && payloads_[i] == key.bytes()) {
        return true;
      }
      i = (i + 1) & mask_;
    }
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  // Slots, not keys: what the two arrays are sized to.
  size_t capacity() const { return capacity_; }

private:
  using Bytes = typename Key::Bytes;

  // An unoccupied slot. A key whose hash is 0 is stored under tag 1; the tag
  // is only a filter ahead of the byte compare, so folding one value onto
  // another costs a compare in 2^-64 of probes and nothing else. Every probe
  // -- insert, lookup, and the reinsert in grow() -- starts from the tag, not
  // the hash, so the two stay in step for that key.
  static constexpr uint64_t kEmpty = 0;
  static constexpr size_t kInitialCapacity = 16;

  static uint64_t tag_of(size_t hash)
  {
    return hash == kEmpty ? 1 : static_cast<uint64_t>(hash);
  }

  // Doubles the arrays and reinserts every occupied slot. Load never exceeds
  // one half, so a linear probe stays short.
  void grow()
  {
    const size_t new_capacity =
        capacity_ == 0 ? kInitialCapacity : capacity_ * 2;
    // Tags zero-initialized (kEmpty); payloads left uninitialized, a slot's
    // bytes are only read once its tag is set.
    auto new_tags = std::make_unique<uint64_t[]>(new_capacity);
    auto new_payloads = std::make_unique_for_overwrite<Bytes[]>(new_capacity);
    const size_t new_mask = new_capacity - 1;
    for (size_t i = 0; i < capacity_; ++i) {
      if (tags_[i] == kEmpty) {
        continue;
      }
      size_t j = slot_of(tags_[i], new_mask);
      while (new_tags[j] != kEmpty) {
        j = (j + 1) & new_mask;
      }
      new_tags[j] = tags_[i];
      new_payloads[j] = payloads_[i];
    }
    tags_ = std::move(new_tags);
    payloads_ = std::move(new_payloads);
    capacity_ = new_capacity;
    mask_ = new_mask;
  }

  static size_t slot_of(uint64_t tag, size_t mask)
  {
    return static_cast<size_t>(tag) & mask;
  }

  std::unique_ptr<uint64_t[]> tags_;
  std::unique_ptr<Bytes[]> payloads_;
  size_t capacity_ = 0;
  size_t mask_ = 0;
  size_t size_ = 0;
};

} // namespace hogd
