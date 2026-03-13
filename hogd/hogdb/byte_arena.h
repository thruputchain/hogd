// -*-C++-*-

#pragma once

#include "x23/nmvec.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>

namespace hogd {

// Per-thread arena allocator for MaterializedChange byte data.
// Allocates from 64KB chunks with bump pointer. Stable addresses.
class ByteArena {
public:
  // Copy bytes into the arena and return a span to the copy.
  std::span<const std::byte> store(std::span<const std::byte> data)
  {
    if (data.empty()) {
      return {};
    }
    auto dest = alloc(data.size());
    std::memcpy(dest.data(), data.data(), data.size());
    return dest;
  }

  // Store an item [key + type_byte + value] contiguously in the arena.
  std::span<const std::byte> store_item(std::span<const std::byte> key,
                                        uint8_t type,
                                        std::span<const std::byte> value)
  {
    size_t total = key.size() + 1 + value.size();
    auto dest = alloc(total);
    if (!key.empty()) {
      std::memcpy(dest.data(), key.data(), key.size());
    }
    dest[key.size()] = static_cast<std::byte>(type);
    if (!value.empty()) {
      std::memcpy(dest.data() + key.size() + 1, value.data(), value.size());
    }
    return dest;
  }

  // Reset position in all chunks (keeps memory for reuse).
  void clear()
  {
    for (auto &chunk : chunks_) {
      chunk.used = 0;
    }
  }

private:
  static constexpr size_t kChunkSize = 64 * 1024;

  struct Chunk {
    std::unique_ptr<std::byte[]> data;
    size_t capacity = 0;
    size_t used = 0;

    Chunk() = default;
    explicit Chunk(size_t cap)
      : data(std::make_unique<std::byte[]>(cap)), capacity(cap)
    {}

    std::span<std::byte> try_alloc(size_t n)
    {
      if (used + n > capacity) {
        return {};
      }
      auto result = std::span(data.get() + used, n);
      used += n;
      return result;
    }
  };

  std::span<std::byte> alloc(size_t n)
  {
    if (chunks_.size() > 0) {
      auto result = chunks_[chunks_.size() - 1].try_alloc(n);
      if (!result.empty()) {
        return result;
      }
    }
    size_t cap = std::max(kChunkSize, n);
    size_t idx = chunks_.emplace_back(cap);
    return chunks_[idx].try_alloc(n);
  }

  x23::NMVec<Chunk> chunks_;
};

} // namespace hogd
