// -*-C++-*-

#pragma once

#include "x23/utility.h"
#include <algorithm>

#include <initializer_list>
#include <utility>
#include <x23/x23.h>

namespace x23 {

// Attempt to deserialize an invalid binary XDR representation.
struct XdrBadInput : XdrException {
  using XdrException::XdrException;
};

template<std::integral T>
inline T
getbe(const void *p)
{
  return [p = static_cast<const std::uint8_t *>(p)]<size_t... I>(
             std::index_sequence<I...>) -> T {
    return ((T(p[I]) << 8 * (sizeof(T) - I - 1)) | ...);
  }(std::make_index_sequence<sizeof(T)>{});
}

template<std::integral T>
inline void
putbe(void *p, T val)
{
  [val, p = static_cast<std::uint8_t *>(p)]<std::size_t... I>(
      std::index_sequence<I...>) {
    ((p[I] = val >> 8 * (sizeof(T) - I - 1)), ...);
  }(std::make_index_sequence<sizeof(T)>{});
}

template<std::integral T, is_bytevec C>
inline void
pushbe(C *out, T val)
{
  out->resize_and_overwrite(out->size() + sizeof(T),
                            [val](typename C::value_type *p, size_t count) {
                              putbe(p + count - sizeof(T), val);
                              return count;
                            });
}

namespace detail {

constexpr size_t kNoBound = std::numeric_limits<size_t>::max();

struct SizeBounds {
  size_t min_;
  size_t max_;

  constexpr SizeBounds(size_t both) noexcept : min_(both), max_(both) {}
  constexpr SizeBounds(size_t min, size_t max) noexcept : min_(min), max_(max)
  {}

  constexpr SizeBounds operator+(size_t n) const noexcept
  {
    return {min_ + n, uplus(max_, n)};
  }
  constexpr SizeBounds operator+(const SizeBounds &other) const noexcept
  {
    return {min_ + other.min_, uplus(max_, other.max_)};
  }
  constexpr SizeBounds operator*(size_t n) const noexcept
  {
    return {umul(min_, n), umul(max_, n)};
  }
  constexpr SizeBounds zero_to(size_t n) const noexcept
  {
    return {0, n >= 0xffffffff ? kNoBound : umul(n, max_)};
  }

  constexpr bool operator==(const SizeBounds &) const noexcept = default;

  static constexpr size_t umul(size_t a, size_t b) noexcept
  {
    if (b == 0)
      return 0;
    if (a > kNoBound / b)
      return kNoBound;
    return a * b;
  }
  static constexpr size_t uplus(size_t a, size_t b) noexcept
  {
    if (b > kNoBound - a)
      return kNoBound;
    return a + b;
  }
};

template<is_xdr_type X, typename... Blacklist>
consteval SizeBounds
xdr_size_bounds()
{
  using enum XdrCategory;
  if constexpr (is_one_of<X, Blacklist...>)
    // This means we have a recursive or mutually-recursive type, so
    // it could go on forever.  (min doesn't matter because in any
    // recursive type there's a Ptr already determining min.)
    return {0, kNoBound};
  else if constexpr (category<X> & FlagSet{kPrimitive, kEnum})
    return sizeof(X) <= 4 ? 4 : 8;
  else if constexpr (category<X> == kOpaqueArray)
    return (std::tuple_size<X>::value + 3UZ) & ~3UZ;
  else if constexpr (category<X> & FlagSet{kString, kOpaqueVec})
    return SizeBounds{0, X::kMaxSize >= 0xffffffff
                             ? kNoBound
                             : (X::kMaxSize + 3UZ) & ~3UZ} +
           4;
  else if constexpr (category<X> == kArray)
    return xdr_size_bounds<typename X::value_type, Blacklist..., X>() *
           std::tuple_size<X>::value;
  else if constexpr (category<X> & FlagSet{kVec, kPtr})
    return xdr_size_bounds<typename X::value_type, Blacklist..., X>().zero_to(
               X::max_size()) +
           4;
  else if constexpr (category<X> == kStruct)
    return with_index_sequence<std::tuple_size_v<X>>([](auto... i) {
      return (SizeBounds{0} + ... +
              xdr_size_bounds<std::tuple_element_t<i, X>, Blacklist..., X>());
    });
  else if constexpr (category<X> == kUnion) {
    using ops = union_ops<X>;
    return with_index_sequence<ops::union_size, 1>([](auto... i) {
      SizeBounds ret{ops::meta::has_void ? 0 : kNoBound, 0};
      for (auto b : {xdr_size_bounds<typename ops::template union_element_t<i>,
                                     Blacklist..., X>()...}) {
        ret.min_ = std::min(ret.min_, b.min_);
        ret.max_ = std::max(ret.max_, b.max_);
      }
      return ret + 4;
    });
  }
}

// True if the size of a marshaled X is independent of its value.
template<is_xdr_type X>
consteval bool
has_fixed_size()
{
  SizeBounds b = xdr_size_bounds<X>();
  return b.min_ == b.max_;
}

template<is_xdr_type X> requires (has_fixed_size<X>())
consteval size_t
size_of_fixed()
{
  return xdr_size_bounds<X>().max_;
}

} // namespace detail

// Number of bytes required to marshal a data structure in XDR format.
template<is_xdr_type X>
constexpr size_t
xdr_size(const X &x)
{
  using enum XdrCategory;
  if constexpr (detail::has_fixed_size<X>())
    return detail::size_of_fixed<X>();
  else if constexpr (category<X> & FlagSet{kString, kOpaqueVec})
    return (x.size() + 7UZ) & ~3UZ;
  else if constexpr (category<X> & FlagSet{kPtr, kVec, kArray}) {
    const size_t sizesize = category<X> == kArray ? 0UZ : 4UZ;
    if constexpr (detail::has_fixed_size<typename X::value_type>())
      return sizesize +
             x.size() * detail::size_of_fixed<typename X::value_type>();
    else
      return std::ranges::fold_left(
          x | std::views::transform([](const auto &x) { return xdr_size(x); }),
          sizesize, std::plus<size_t>{});
  }
  else if constexpr (category<X> == kUnion)
    return 4 +
           union_visit_arm([](const auto &a) { return xdr_size(a); }, x, 0UZ);
  else if constexpr (category<X> == kStruct)
    return with_index_sequence<std::tuple_size_v<X>>(
        [&x](auto... i) { return (xdr_size(get<i>(x)) + ...); });
}

void
serialize_to(Buf *bufp, const is_xdr_type auto &x)
{
  decltype(bufp->data()) p;
  auto go = [&p]<is_xdr_type X>(this auto &&go, const X &x) -> void {
    using enum XdrCategory;
    using std::uint32_t;
    if constexpr (category<X> & FlagSet{kPrimitive, kEnum}) {
      using type = std::conditional_t<sizeof(X) <= 4, uint32_t, std::uint64_t>;
      putbe(p, type(x));
      p += sizeof(type);
    }
    else if constexpr (category<X> &
                       FlagSet{kString, kOpaqueVec, kOpaqueArray}) {
      uint32_t sz = x.size();
      if constexpr (category<X> != kOpaqueArray)
        go(sz);
      memcpy(p, x.data(), sz);
      p += sz;
      if ((sz = sz % 4)) {
        sz = 4 - sz;
        memset(p, '\0', sz);
        p += sz;
      }
    }
    else if constexpr (category<X> == kPtr) {
      go(bool(x));
      if (x)
        go(*x);
    }
    else if constexpr (category<X> & FlagSet{kVec, kArray}) {
      if (category<X> == kVec)
        go(uint32_t(x.size()));
      for (const auto &y : x)
        go(y);
    }
    else if constexpr (category<X> == kStruct) {
      with_index_sequence<std::tuple_size_v<X>>(
          [&go, &x](auto... i) { (go(get<i>(x)), ...); });
    }
    else if constexpr (category<X> == kUnion) {
      if (!union_valid(x))
        throw XdrBadUnionAccess(
            "Attempt to serialize {} with invalid discrminant {}",
            xdr_meta<X>::type_name, get<0>(x));
      go(get<0>(x));
      union_visit_arm(go, x);
    }
  };

  size_t old_size = bufp->size(), new_size = old_size + xdr_size(x);
  bufp->resize_and_overwrite(new_size,
                             [&go, &x, &p, old_size](auto *data, size_t count) {
                               p = data + old_size;
                               go(x);
                               assert(p == data + count);
                               return count;
                             });
}

Buf
serialize(const is_xdr_type auto &...x)
{
  Buf ret;
  (serialize_to(&ret, x), ...);
  return ret;
}

template<is_xdr_type Xout>
size_t
deserialize_from(Xout *out, BufView buf)
{
  if (buf.size() % 4)
    throw XdrBadInput("input not multiple of 4 bytes deserializing {}",
                      xdr_type_name<Xout>());

  const std::uint8_t *p = buf.data();
  const std::uint8_t *const e = p + buf.size();

#define NEED(n)                                                                \
  do {                                                                         \
    if (size_t(e - p) < (n)) [[unlikely]]                                      \
      throw XdrBadInput("buffer overrun deserializing {} in {}",               \
                        xdr_type_name<X>(), xdr_type_name<Xout>());            \
  } while (0)

  auto go = [&p, e]<typename X>(this auto &&go, X &x) -> void {
    using enum XdrCategory;
    if constexpr (category<X> & FlagSet{kPrimitive, kEnum}) {
      using uint_type =
          std::conditional_t<sizeof(X) <= 4, std::uint32_t, std::uint64_t>;
      NEED(sizeof(uint_type));
      auto val = getbe<uint_type>(p);
      p += sizeof(uint_type);
      if constexpr (sizeof(x) == sizeof(val))
        x = std::bit_cast<X>(val); // works for float, double
      else {                       // works for bool, maybe some enums?
        if constexpr (std::same_as<X, bool>)
          if (val > 1)
            throw XdrBadInput("illegal bool value {} deserializing {} in {}",
                              val, xdr_type_name<X>(), xdr_type_name<Xout>());
        x = X(val);
      }
    }
    else if constexpr (category<X> &
                       FlagSet{kString, kOpaqueVec, kOpaqueArray}) {
      std::uint32_t memlen;
      if constexpr (category<X> == kOpaqueArray)
        memlen = x.size();
      else {
        go(memlen);
        if (memlen > x.max_size()) [[unlikely]]
          throw XdrBadInput("length {} exceeds maximum deserializing {}",
                            memlen, xdr_type_name<X>());
      }
      std::size_t binlen = (memlen + 3UZ) & ~3UZ;
      NEED(binlen);
      if constexpr (category<X> != kOpaqueArray)
        x.resize_uninitialized(memlen);
      for (size_t i = memlen; i < binlen; ++i)
        if (p[i])
          throw XdrBadInput("pad bytes are not zero deserializing {}",
                            xdr_type_name<X>());
      memcpy(x.data(), p, memlen);
      p += binlen;
    }
    else if constexpr (category<X> & FlagSet{kVec, kPtr}) {
      std::uint32_t len;
      go(len);
      if (len > x.max_size()) [[unlikely]]
        throw XdrBadInput("length {} exceeds maximum deserializing {}", len,
                          xdr_type_name<X>());
      x.resize(0);
      // don't let bad input exhaust memory
      x.reserve(std::min<std::uint32_t>(len, 100));
      for (size_t i = 0; i < len; ++i) {
        x.resize(i + 1);
        go(x.back());
      }
    }
    else if constexpr (category<X> == kArray)
      for (auto &e : x)
        go(e);
    else if constexpr (category<X> == kStruct) {
      with_index_sequence<std::tuple_size_v<X>>(
          [&go, &x](auto... i) { (go(get<i>(x)), ...); });
    }
    else if constexpr (category<X> == kUnion) {
      using ops = union_ops<X>;
      typename ops::discriminant_type d;
      go(d);
      union_set_discriminant(x, d);
      if (ops::index(d) == -1) [[unlikely]]
        throw XdrBadInput("Bad discriminant {} deserializing {}", d,
                          ops::meta::type_name);
      union_visit_arm(go, x);
    }
  };
#undef NEED

  go(*out);
  return p - buf.data();
}

template<is_xdr_type Xout>
inline void
deserialize(Xout *out, BufView buf)
{
  if (deserialize_from(out, buf) != buf.size())
    throw XdrBadInput("leftover bytes deserializing {}", xdr_type_name<Xout>());
}

} // namespace x23
