#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "types.hh"
#include "x23/x23.h"

namespace hogd {

using DbHash = StorageLongHash;
using BlockHash = ProposalHeaderHash;

struct BlockHeader {
  uint64_t network_id;
  BlockHeight block_height;
  SHA256Hash txset_hash;
  DbHash db_hash;
  SHA256Hash events_hash;
  BlockHash prev_block_hash;

  bool operator==(const BlockHeader &) const noexcept = default;
  friend std::partial_ordering
  operator<=>(const BlockHeader &, const BlockHeader &) noexcept;
};

template<size_t I, x23::references<BlockHeader> T>
requires(I < 6) decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).network_id);
  if constexpr (I == 1)
    return (std::forward<T>(t).block_height);
  if constexpr (I == 2)
    return (std::forward<T>(t).txset_hash);
  if constexpr (I == 3)
    return (std::forward<T>(t).db_hash);
  if constexpr (I == 4)
    return (std::forward<T>(t).events_hash);
  if constexpr (I == 5)
    return (std::forward<T>(t).prev_block_hash);
}

} // namespace hogd

template<>
struct std::tuple_size<hogd::BlockHeader>
  : std::integral_constant<std::size_t, 6> {};

template<std::size_t I>
requires(I < 6) struct std::tuple_element<I, hogd::BlockHeader> {
  using struct_type = hogd::BlockHeader;
  using type = decltype(auto(get<I>(struct_type{})));
};

template<>
struct x23::xdr_meta<hogd::BlockHeader> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "BlockHeader";
  static constexpr std::array<std::string_view, 6> field_names{
    "network_id", "block_height", "txset_hash",
    "db_hash",    "events_hash",  "prev_block_hash",
  };
};

namespace hogd {

inline std::partial_ordering
operator<=>(const BlockHeader &a, const BlockHeader &b) noexcept
{
  return x23::xdr_cmp(a, b);
}

} // namespace hogd
