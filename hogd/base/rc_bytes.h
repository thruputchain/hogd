#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace hogd {

// Immutable bytes with a non-atomic reference count, in one allocation. Copies
// share the bytes, so all of them must stay on one thread at a time.
class RcBytes {
public:
  // An empty RcBytes, which allocates nothing.
  RcBytes() = default;

  // Allocates a buffer holding a copy of bytes.
  explicit RcBytes(std::span<const uint8_t> bytes)
    : header_(
          static_cast<Header *>(::operator new(sizeof(Header) + bytes.size())))
  {
    std::construct_at(header_, Header{.count = 1, .size = bytes.size()});
    if (!bytes.empty()) {
      std::memcpy(bytes_of(header_), bytes.data(), bytes.size());
    }
  }

  RcBytes(RcBytes const &other) noexcept : header_(other.header_)
  {
    if (header_) {
      ++header_->count;
    }
  }
  RcBytes(RcBytes &&other) noexcept
    : header_(std::exchange(other.header_, nullptr))
  {}

  RcBytes &operator=(RcBytes const &other) noexcept
  {
    if (this != &other) {
      RcBytes(other).swap(*this);
    }
    return *this;
  }
  RcBytes &operator=(RcBytes &&other) noexcept
  {
    RcBytes(std::move(other)).swap(*this);
    return *this;
  }

  ~RcBytes()
  {
    if (header_ && --header_->count == 0) {
      ::operator delete(header_, sizeof(Header) + header_->size);
    }
  }

  std::span<const uint8_t> span() const
  {
    if (!header_) {
      return {};
    }
    return {bytes_of(header_), header_->size};
  }
  size_t size() const { return header_ ? header_->size : 0; }

  // The number of RcBytes sharing these bytes (0 if this one is empty).
  size_t use_count() const { return header_ ? header_->count : 0; }

  void swap(RcBytes &other) noexcept { std::swap(header_, other.header_); }

private:
  struct Header {
    size_t count;
    size_t size;
  };

  static uint8_t *bytes_of(Header *header)
  {
    return reinterpret_cast<uint8_t *>(header) + sizeof(Header);
  }

  Header *header_ = nullptr;
};

} // namespace hogd
