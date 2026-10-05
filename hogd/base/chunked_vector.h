#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/log.h"

namespace hogd {

// A sequence of items kept in several chunks, such as the parts of a run a
// ChunkRunAssembler reassembled, read as if it were one vector: it iterates,
// indexes and counts the items across the chunks in order, and never copies
// them into one buffer. [Chunk] is a vector-like container of the items, with
// value_type, size() and operator[]. The chunks stay reachable through
// chunks() for a consumer that works chunk by chunk.
template<typename Chunk> class ChunkedVector {
public:
  using value_type = typename Chunk::value_type;

  // Walks the items of every chunk in order. Equality compares the item
  // position, so iterators at the same item compare equal however they were
  // reached.
  class ConstIterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = ChunkedVector::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type *;
    using reference = const value_type &;

    ConstIterator() = default;

    reference operator*() const { return owner_->chunks_[chunk_][offset_]; }
    pointer operator->() const { return &**this; }

    ConstIterator &operator++()
    {
      ++offset_;
      skip_exhausted_chunks();
      return *this;
    }

    ConstIterator operator++(int)
    {
      ConstIterator before = *this;
      ++*this;
      return before;
    }

    bool operator==(const ConstIterator &other) const
    {
      return chunk_ == other.chunk_ && offset_ == other.offset_;
    }

  private:
    friend class ChunkedVector;

    ConstIterator(const ChunkedVector *owner, size_t chunk, size_t offset)
      : owner_(owner), chunk_(chunk), offset_(offset)
    {
      skip_exhausted_chunks();
    }

    // Moves past the end of the current chunk, and past any empty chunk after
    // it, to the next item; the end iterator is past the last chunk.
    void skip_exhausted_chunks()
    {
      while (chunk_ < owner_->chunks_.size() &&
             offset_ >= owner_->chunks_[chunk_].size()) {
        ++chunk_;
        offset_ = 0;
      }
    }

    const ChunkedVector *owner_ = nullptr;
    size_t chunk_ = 0;
    size_t offset_ = 0;
  };

  using const_iterator = ConstIterator;
  using iterator = ConstIterator;

  ChunkedVector() = default;

  // Takes the chunks in the order their items are to be read.
  explicit ChunkedVector(std::vector<Chunk> chunks) : chunks_(std::move(chunks))
  {
    ends_.reserve(chunks_.size());
    size_t end = 0;
    for (const Chunk &chunk : chunks_) {
      end += chunk.size();
      ends_.push_back(end);
    }
  }

  // The number of items across every chunk.
  size_t size() const { return ends_.empty() ? 0 : ends_.back(); }
  bool empty() const { return size() == 0; }

  const_iterator begin() const { return const_iterator(this, 0, 0); }
  const_iterator end() const { return const_iterator(this, chunks_.size(), 0); }

  // The item at [index] across the chunks; [index] must be below size().
  const value_type &operator[](size_t index) const
  {
    return *iterator_at(index);
  }

  // An iterator at the item at [index] across the chunks, or end() when
  // [index] is size(). Finds the chunk by binary search, so a consumer that
  // splits the items into ranges positions itself once per range and then
  // steps.
  const_iterator iterator_at(size_t index) const
  {
    CHECK_LE(index, size());
    if (index == size()) {
      return end();
    }
    // The first chunk whose items reach past [index] holds it.
    const auto chunk_end = std::ranges::upper_bound(ends_, index);
    const size_t chunk = static_cast<size_t>(chunk_end - ends_.begin());
    const size_t chunk_begin = chunk == 0 ? 0 : ends_[chunk - 1];
    return const_iterator(this, chunk, index - chunk_begin);
  }

  // The chunks, in item order; some may be empty.
  std::span<const Chunk> chunks() const { return chunks_; }

private:
  std::vector<Chunk> chunks_;
  // ends_[i] is the number of items in chunks_[0..i], so the position just
  // past chunk i.
  std::vector<size_t> ends_;
};

} // namespace hogd
