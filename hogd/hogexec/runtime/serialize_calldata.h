#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {

// Group methods for de/serializing calldata into once location, with a common
// interface.
enum class SerializationFormat { kTxs, kCxs };

namespace detail {
template<typename... Args>
using deserialize_return_t = std::optional<std::conditional_t<
    sizeof...(Args) == 1,
    std::tuple_element_t<0, std::tuple<std::remove_cvref_t<Args>...>>,
    std::tuple<std::remove_cvref_t<Args>...>>>;
} // namespace detail

template<SerializationFormat Format> struct Serializer;

template<> struct Serializer<SerializationFormat::kTxs> {
  template<typename... Args>
  static std::optional<Calldata> serialize(Args const &...args)
  {
    try {
      return x23::serialize(args...);
    } catch (const x23::XdrBadInput &e) {
      trace("TXS Serialize failed: {}", e.what());
      return std::nullopt;
    }
  }

  template<typename... Args>
  static auto deserialize(std::span<const std::byte> data)
      -> detail::deserialize_return_t<Args...>
  {
    auto args = std::tuple<std::remove_cvref_t<Args>...>{};
    try {
      x23::deserialize(&args, x23::BufView{data});
    } catch (const x23::XdrBadInput &e) {
      trace("TXS Deserialize failed: {}", e.what());
      return std::nullopt;
    }

    if constexpr (sizeof...(Args) == 1) {
      return std::get<0>(args);
    }
    else {
      return args;
    }
  }
};

template<> struct Serializer<SerializationFormat::kCxs> {
  template<typename... Args>
  static std::optional<Calldata> serialize(Args const &...args)
  {
    try {
      return x23::serialize_cxs(args...);
    } catch (const x23::XdrBadInput &e) {
      trace("CXS Serialize failed: {}", e.what());
      return std::nullopt;
    }
  }

  template<typename... Args>
  static auto deserialize(std::span<const std::byte> data)
      -> detail::deserialize_return_t<Args...>
  {
    auto args = std::tuple<std::remove_cvref_t<Args>...>{};
    try {
      x23::deserialize_cxs(&args,
                           {reinterpret_cast<const uint8_t *>(data.data()),
                            data.size()});
    } catch (const x23::XdrBadInput &e) {
      trace("CXS Deserialize failed: {}", e.what());
      return std::nullopt;
    }
    if constexpr (sizeof...(Args) == 1) {
      return std::get<0>(args);
    }
    else {
      return args;
    }
  }
};

} // namespace hogd
