#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// The items of one chunk of a run (see ChunkPosition), as spans into the
// vectors they were produced in, so a call serializes them in place instead
// of copying them into a vector first. Serializes exactly like an XDR vector
// of T: a length prefix, then the items piece by piece. It is valid only
// until the call it was handed to resolves; call_in_chunks keeps the parts
// alive until then.
template<typename T> class ChunkItems {
public:
  explicit ChunkItems(std::span<const std::span<const T>> pieces)
    : pieces_(pieces)
  {
    for (const std::span<const T> &piece : pieces_) {
      size_ += piece.size();
    }
  }

  std::span<const std::span<const T>> pieces() const { return pieces_; }
  size_t size() const { return size_; }

private:
  std::span<const std::span<const T>> pieces_;
  size_t size_ = 0;
};

template<x23::is_xdr_serializable T>
size_t
xdr_size(const ChunkItems<T> &items)
{
  if constexpr (requires {
                  {
                    x23::detail::size_of_fixed<T>()
                  } -> std::convertible_to<size_t>;
                }) {
    return 4 + (items.size() * x23::detail::size_of_fixed<T>());
  }
  size_t ret = 4;
  for (const std::span<const T> &piece : items.pieces()) {
    for (const T &item : piece) {
      ret += x23::xdr_size(item);
    }
  }
  return ret;
}

template<x23::is_xdr_serializable T>
void
xdr_serialize(auto &serializer, const ChunkItems<T> &items)
{
  serializer.put32(static_cast<std::uint32_t>(items.size()));
  for (const std::span<const T> &piece : items.pieces()) {
    for (const T &item : piece) {
      serializer(item);
    }
  }
}

} // namespace hogd

// ChunkItems<T> may stand in wherever the wire type is a vector of T.
template<typename X, size_t N, typename T> requires x23::serializes_like<T, X>
struct x23::serialize_equiv<x23::Vec<X, N>, hogd::ChunkItems<T>>
  : std::true_type {};
