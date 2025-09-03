#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

#include "bcs/decode.h"
#include "bcs/encode.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"

namespace hogd {

// Group methods for de/serializing calldata into once location, with a common
// interface.
enum class SerializationFormat { kBcs, kTxs, kCxs };

namespace detail {
template<typename... Args>
using deserialize_return_t = std::optional<std::conditional_t<
    sizeof...(Args) == 1,
    std::tuple_element_t<0, std::tuple<std::remove_cvref_t<Args>...>>,
    std::tuple<std::remove_cvref_t<Args>...>>>;
} // namespace detail

template<SerializationFormat Format> struct Serializer;

template<> struct Serializer<SerializationFormat::kBcs> {

  template<typename... Args>
  static std::optional<Calldata> serialize(Args const &...args)
  {
    auto r = bcs::encode_to<Calldata>(args...);
    if (!r.has_value()) {
      return std::nullopt;
    }
    return r.value();
  }

  template<typename... Args>
  static auto deserialize(std::span<const std::byte> data)
      -> detail::deserialize_return_t<Args...>
  {
    if constexpr (sizeof...(Args) == 1) {
      auto r = bcs::decode_from<Args...>(data);
      if (!r.has_value()) {
        return std::nullopt;
      }
      return std::get<0>(r.value());
    }
    else {
      auto r = bcs::decode_from<std::tuple<std::remove_cvref_t<Args>...>>(data);
      if (!r.has_value()) {
        return std::nullopt;
      }
      // the bcs library is a bit weird, and returns a tuple of tuples
      // TODO(geoff): fix, eventually, if we use bcs
      // There might be a way to avoid this using the parameter packs,
      // but the error handling gets messy and this works
      return std::get<0>(std::get<0>(r.value()));
    }
  }
};

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
