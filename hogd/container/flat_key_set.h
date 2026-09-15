#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace hogd {

// flat_key_set.h - an insert-only, open-addressed set of keys that carry
// their own hash. Built for SystemCallHandler::loaded_keys_, the per-tx set
// behind the first-touch load charge, where std::unordered_set's node layout
// cost two dependent cache misses per probe.
//
// Hashes live in one contiguous array and key bytes in another, so a probe
// reads the small hash array first and touches key bytes only on a match.
// Membership is exact: the hash chooses where to look, the byte compare
// decides equality, so a collision costs a compare, never an under-charge.
// No erase and no iteration.
//
// Key requirements: `key.hash()` returning a size_t, and `key.bytes()`
// returning a `Key::Bytes const &`, the std::array equality is decided on.
// FrameItemKey is the intended key.
template<typename Key> class FlatKeySet {
public:
  FlatKeySet() = default;

  // Leave the source empty and reusable after moving its storage.
  FlatKeySet(FlatKeySet &&other) noexcept
    : tags_(std::move(other.tags_)), payloads_(std::move(other.payloads_)),
      capacity_(std::exchange(other.capacity_, 0)),
      mask_(std::exchange(other.mask_, 0)), size_(std::exchange(other.size_, 0))
  {}

  FlatKeySet &operator=(FlatKeySet &&other) noexcept
  {
    if (this != &other) {
      tags_ = std::move(other.tags_);
      payloads_ = std::move(other.payloads_);
      capacity_ = std::exchange(other.capacity_, 0);
      mask_ = std::exchange(other.mask_, 0);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }
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

  // Tag value marking an unoccupied slot; a hash of 0 is stored as 1. Every
  // probe, grow()'s reinsert included, starts from the tag, not the hash.
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
