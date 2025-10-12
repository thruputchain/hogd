// -*-C++-*-

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <initializer_list>
#include <limits>
#include <print>
#include <string>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

#include <x23/utility.h>
#include <x23/vec.h>
#include <x23/x23.h>

namespace x23 {

// Attempt to deserialize an invalid binary XDR representation.
struct XdrBadInput : XdrException {
  using XdrException::XdrException;
};

// Load an integral from memory in big-endian order.  Makes no
// assumptions about the alignment of p.  gcc and clang can often
// compile this to a single movebe instruction on x86, or else a load
// followed by a bswap.  So this is at least as good as doing
// std::byteswap(*static_cast<T*>(p)) on endian x86, while also being
// correct on architectures with greater pointer-alignment
// constraints.  (As a bonus, we don't need a separate, untested
// version of the code for big-endian machines.)
template<std::integral T>
inline T
getbe(const void *p)
{
  return [p = static_cast<const uint8_t *>(p)]<size_t... I>(
             std::index_sequence<I...>) -> T {
    return ((T(p[I]) << 8 * (sizeof(T) - I - 1)) | ...);
  }(std::make_index_sequence<sizeof(T)>{});
}

// Store an integral into memory in big-endian order, with no
// assumptions about pointer alignment.  See the comment on getbe.
template<std::integral T>
inline void
putbe(void *p, T val)
{
  [val,
   p = static_cast<uint8_t *>(p)]<std::size_t... I>(std::index_sequence<I...>) {
    ((p[I] = val >> 8 * (sizeof(T) - I - 1)), ...);
  }(std::make_index_sequence<sizeof(T)>{});
}

namespace detail {

constexpr size_t kNoBound = std::numeric_limits<std::ptrdiff_t>::max();

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
    return {min_ * n, umul(max_, n)};
  }
  constexpr SizeBounds zero_to(size_t n) const noexcept
  {
    return {0, umul(n, max_)};
  }

  constexpr bool operator==(const SizeBounds &) const noexcept = default;

  static constexpr size_t umul(size_t a, size_t b) noexcept
  {
    if (a == 0)
      return 0;
    if (b > kNoBound / a)
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
    // recursive type there's a Ptr or Vec already determining min.)
    return {0, kNoBound};
  else if constexpr (is_xdr_cat<X, kPrimitive, kEnum>)
    return sizeof(X) <= 4 ? 4 : 8;
  else if constexpr (is_xdr_cat<X, kOpaqueArray>)
    return (std::tuple_size<X>::value + 3UZ) & ~3UZ;
  else if constexpr (is_xdr_cat<X, kString, kOpaqueVec>)
    return SizeBounds{0, X::kMaxSize >= std::min<size_t>(0xfffffffd, kNoBound)
                             ? kNoBound
                             : (X::kMaxSize + 3UZ) & ~3UZ} +
           4;
  else if constexpr (is_xdr_cat<X, kArray>)
    return xdr_size_bounds<typename X::value_type, Blacklist..., X>() *
           std::tuple_size<X>::value;
  else if constexpr (is_xdr_cat<X, kVec, kPtr>)
    return xdr_size_bounds<typename X::value_type, Blacklist..., X>().zero_to(
               X::max_size()) +
           4;
  else if constexpr (is_xdr_struct<X>)
    return []<size_t... I>(std::index_sequence<I...>) {
      return (SizeBounds{0} + ... +
              xdr_size_bounds<std::tuple_element_t<I, X>, Blacklist..., X>());
    }(std::make_index_sequence<std::tuple_size_v<X>>{});
  else if constexpr (is_xdr_union<X>) {
    using ops = union_ops<X>;
    return []<size_t... I>(std::index_sequence<I...>) {
      SizeBounds ret{ops::meta::has_void ? 0 : kNoBound, 0};
      for (auto b : std::initializer_list<SizeBounds>{
               xdr_size_bounds<typename ops::template union_element_t<I + 1>,
                               Blacklist..., X>()...}) {
        ret.min_ = std::min(ret.min_, b.min_);
        ret.max_ = std::max(ret.max_, b.max_);
      }
      return ret + 4;
    }(std::make_index_sequence<ops::union_size - 1>{});
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

template<is_xdr_struct X>
constexpr auto kMaxFieldSize = []<size_t... I>(std::index_sequence<I...>) {
  return std::array<size_t, sizeof...(I)>{
      xdr_size_bounds<std::tuple_element_t<I, X>>().max_...};
}(std::make_index_sequence<std::tuple_size_v<X>>{});

template<is_xdr_struct X>
consteval size_t
first_unsafe_field(size_t nbytes, size_t cur_field = 0)
{
  while (cur_field < kMaxFieldSize<X>.size()) {
    if (size_t n = kMaxFieldSize<X>[cur_field]; n <= nbytes)
      nbytes = -n;
    else
      break;
    cur_field++;
  }
  return cur_field;
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
  else if constexpr (is_xdr_cat<X, kString, kOpaqueVec>)
    return (x.size() + 7UZ) & ~3UZ;
  else if constexpr (is_xdr_cat<X, kPtr, kVec, kArray>) {
    constexpr size_t sizesize = category<X> == kArray ? 0UZ : 4UZ;
    if constexpr (detail::has_fixed_size<typename X::value_type>())
      return sizesize +
             x.size() * detail::size_of_fixed<typename X::value_type>();
    else {
      size_t size = sizesize;
      for (const auto &e : x)
        size += xdr_size(e);
      return size;
    }
  }
  else if constexpr (is_xdr_union<X>)
    return 4 +
           union_visit_arm([](const auto &a) { return xdr_size(a); }, x, 0UZ);
  else if constexpr (is_xdr_struct<X>) {
    size_t size = 0;
    visit_fields([&size](const auto &x) { size += xdr_size(x); }, x);
    return size;
  }
}

namespace detail {
struct Serializer {
  uint8_t *p;

  void put32(uint32_t v)
  {
    putbe(p, v);
    p += 4;
  }
  void put64(uint64_t v)
  {
    putbe(p, v);
    p += 8;
  }
  void putbytes(const void *buf, size_t len)
  {
    if (size_t rem = len % 4) {
      *reinterpret_cast<uint32_t *>(p + len - rem) = 0;
      memcpy(p, buf, len);
      p += len + (4 - rem);
    }
    else {
      memcpy(p, buf, len);
      p += len;
    }
  }

  void operator()(bool b) { put32(b); }
  void operator()(is_xdr_cat<XdrCategory::kPrimitive> auto x)
      requires (sizeof(x) == 4)
  {
    put32(std::bit_cast<uint32_t>(x));
  }
  void operator()(is_xdr_cat<XdrCategory::kPrimitive> auto x)
      requires (sizeof(x) == 8)
  {
    put64(std::bit_cast<uint64_t>(x));
  }
  void operator()(is_xdr_enum auto e) { put32(uint32_t(e)); }

  template<is_xdr_cat<XdrCategory::kOpaqueVec, XdrCategory::kString> X>
  void operator()(const X &s)
  {
    size_t len = s.size();
    put32(len);
    putbytes(s.data(), len);
  }
  template<is_xdr_cat<XdrCategory::kOpaqueArray> X> void operator()(const X &s)
  {
    putbytes(s.data(), s.size());
  }

  template<is_xdr_cat<XdrCategory::kVec> X> void operator()(const X &v)
  {
    put32(v.size());
    for (const auto &e : v)
      (*this)(e);
  }
  template<is_xdr_cat<XdrCategory::kPtr> X> void operator()(const X &v)
  {
    if (v) {
      put32(1);
      (*this)(*v);
    }
    else
      put32(0);
  }
  void operator()(const is_xdr_cat<XdrCategory::kArray> auto &a)
  {
    for (const auto &e : a)
      (*this)(e);
  }

  template<is_xdr_struct X> void operator()(const X &x)
  {
    visit_fields(*this, x);
  }
  template<is_xdr_union X> void operator()(const X &u)
  {
    if (!union_valid(u)) [[unlikely]]
      throw XdrBadUnionAccess(
          "Attempt to serialize {} with invalid discrminant {}",
          xdr_meta<X>::type_name, get<0>(u));
    put32(uint32_t(get<0>(u)));
    union_visit_arm(*this, u);
  }
};
} // namespace detail

void
serialize_to(Buf *bufp, const is_xdr_type auto &x)
{
  size_t old_size = bufp->size(), xsize = xdr_size(x);
  bufp->resize_uninitialized(old_size + xsize);
  detail::Serializer{.p = bufp->data() + old_size}(x);
}

Buf
serialize(const is_xdr_type auto &...x)
{
  Buf ret;
  (serialize_to(&ret, x), ...);
  return ret;
}

// This is like a BufView, but where it might be safe to read some
// number of bytes beyond the end of the buffer to bufend().
class BufViewEnd : public BufView {
public:
  BufViewEnd(const Buf &b) noexcept
    : BufView(b), bufend_(b.data() + b.capacity())
  {}
  BufViewEnd(BufView b) noexcept : BufView(b), bufend_{data() + size()} {}
  BufViewEnd(BufView b, const uint8_t *be) noexcept : BufView(b), bufend_(be) {}
  BufViewEnd(BufView b, const Buf &container) noexcept
    : BufView(b), bufend_(container.data() + container.capacity())
  {
#ifndef NDEBUG
    if (!empty()) {
      assert(data() >= container.data());
      assert(data() + size() <= container.data() + container.size());
    }
#endif // !NDEBUG
  }

  const uint8_t *bufend() const noexcept { return bufend_; }

private:
  const uint8_t *bufend_;
};

namespace detail {

// Dummy type representing a set of types
template<typename... T> struct type_sequence {};

// Get the types of all fields in a struct or union, or the elements
// of a vector/array/pointer, represented as a type_sequence.
template<is_xdr_type X>
consteval auto
get_field_types()
{
  using enum XdrCategory;
  if constexpr (is_xdr_cat<X, kPtr, kVec, kArray>)
    return type_sequence<typename X::value_type>{};
  else if constexpr (is_xdr_cat<X, kStruct>)
    return []<size_t... I>(std::index_sequence<I...>) {
      return type_sequence<std::tuple_element_t<I, X>...>{};
    }(std::make_index_sequence<std::tuple_size_v<X>>{});
  else if constexpr (is_xdr_cat<X, kUnion>) {
    using ops = union_ops<X>;
    return []<size_t... I>(std::index_sequence<I...>) {
      return type_sequence<typename ops::template union_element_t<I>...>{};
    }(std::make_index_sequence<ops::union_size>{});
  }
  else
    return type_sequence<>{};
}

// True if an instance of Inner can be found anywhere inside an Outer
// or, recursively, inside any field of Outer.
template<is_xdr_type Outer, is_xdr_type Inner = Outer, typename... Stop>
consteval bool
xdr_contains()
{
  // Prevent infinite recursion
  if constexpr (is_one_of<Outer, Stop...>)
    return false;
  else
    return []<typename... T>(type_sequence<T...>) {
      if constexpr (is_one_of<Inner, T...>)
        return true;
      else
        return (xdr_contains<T, Inner, Stop..., Outer>() || ...);
    }(get_field_types<Outer>());
}

template<is_xdr_type X, typename... Stop>
consteval bool
xdr_contains_recursive()
{
  if constexpr (is_one_of<X, Stop...>)
    return true;
  else
    return []<typename... T>(type_sequence<T...>) {
      return (xdr_contains_recursive<T, Stop..., X>() || ...);
    }(get_field_types<X>());
}

} // namespace detail

// True if type X is an XDR struct or union that contains itself
// recursively.
template<typename X>
concept is_recursive_xdr =
    is_xdr_cat<X, XdrCategory::kStruct, XdrCategory::kUnion> &&
    detail::xdr_contains<X>();

template<typename X>
concept contains_recursive_xdr =
    is_xdr_type<X> && detail::xdr_contains_recursive<X>();

// Default number of stack frames allowed corresponding to recursive
// structs and unions.
extern size_t deserialization_recursion_limit;

namespace detail {

struct DeserializationRecursionChecker {
  size_t recursion_limit_ = deserialization_recursion_limit;
  void recurse(this auto &&self)
  {
    if (!self.recursion_limit_) [[unlikely]]
      self.raise("x23::Deserializer: exceeded recursion limit of {}",
                 deserialization_recursion_limit);
    --self.recursion_limit_;
  }
};

template<bool CheckRecursion>
struct Deserializer
  : public std::conditional_t<CheckRecursion, DeserializationRecursionChecker,
                              std::monostate> {
  const uint8_t *p;
  const uint8_t *const e;  // end of valid input (might read beyond)
  const uint8_t *const ee; // end of buffer (shouldn't read beyond)

  Deserializer(BufViewEnd b) noexcept
    : Deserializer(b.data(), b.data() + b.size(), b.bufend())
  {}
  Deserializer(const uint8_t *p0, const uint8_t *e0,
               const uint8_t *ee0) noexcept
    : p(p0), e(e0), ee(ee0)
  {}

  // Check for buffer overrun at end of deserialization--must not use
  // Deserializer without eventually calling this.  (Since it throws,
  // we don't want to put it in the destructor.)
  void check() const
  {
    if (p > e) [[unlikely]] {
      if (p > ee) {
        // We may have read beyond the buffer, crash whole program
        static const char msg[] = "x23::Deserializer: buffer overrun";
        write(2, msg, sizeof(msg) - 1);
        std::terminate();
      }
      std::println("input too short B");
      throw XdrBadInput("x23::Deserializer: input too short");
    }
  }

  void need(size_t n) const
  {
    if constexpr (sizeof(size_t) == 4) {
      if (n > PTRDIFF_MAX)
        throw XdrBadInput("x23::Deserializer: size too large");
    }
    if (e - p < std::ptrdiff_t(n)) [[unlikely]] {
      std::println("input too short C");
      throw XdrBadInput("x23::Deserializer: input too short");
    }
  }

  template<bool Safe> uint32_t get32()
  {
    if constexpr (Safe)
      need(4);
    auto ret = getbe<uint32_t>(p);
    p += 4;
    return ret;
  }
  template<bool Safe> uint64_t get64()
  {
    if constexpr (Safe)
      need(8);
    auto ret = getbe<uint64_t>(p);
    p += 8;
    return ret;
  }

  template<bool Safe> void go(bool &b)
  {
    if (auto val = get32<Safe>(); val > 1)
      throw XdrBadInput("illegal bool value {} in deserialization", val);
    else
      b = val;
  }
  template<bool Safe, is_xdr_cat<XdrCategory::kPrimitive, XdrCategory::kEnum> X>
  void go(X &x)
  {
    static_assert(sizeof(x) == 4 || sizeof(x) == 8);
    if constexpr (is_xdr_cat<X, XdrCategory::kEnum>)
      x = static_cast<X>(get32<Safe>());
    else if constexpr (sizeof(x) == 4)
      x = std::bit_cast<X>(get32<Safe>());
    else
      x = std::bit_cast<X>(get64<Safe>());
  }

  void check_trailing_zeros(size_t rem) const
  {
    constexpr auto swaple = [](uint32_t v) consteval -> uint32_t {
      return std::endian::native == std::endian::little ? std::byteswap(v) : v;
    };
    constexpr std::array masks{swaple(0), swaple(0xffffff), swaple(0xffff),
                               swaple(0xff)};
    if (reinterpret_cast<const uint32_t *>(p)[-1] & masks[rem]) [[unlikely]]
      raise("padding bytes are not zero deserializing XDR");
  }
  template<bool Safe, is_xdr_cat<XdrCategory::kOpaqueArray> X> void go(X &x)
  {
    constexpr size_t size = std::tuple_size_v<X>, rem = size % 4;
    if constexpr (rem == 0) {
      if constexpr (Safe)
        need(x.size());
      if constexpr (size > 0) {
        memcpy(x.data(), p, size);
        p += size;
      }
    }
    else {
      constexpr size_t binlen = (x.size() + 3UZ) & ~3UZ;
      static_assert(binlen > 0);
      if constexpr (Safe)
        need(binlen);
      memcpy(x.data(), p, size);
      p += binlen;
      check_trailing_zeros(rem);
    }
  }
  template<bool Safe,
           is_xdr_cat<XdrCategory::kString, XdrCategory::kOpaqueVec> X>
  void go(X &x)
  {
    uint32_t size = get32<Safe>();
    if (size > x.max_size()) [[unlikely]]
      raise("length {} exceeds maximum deserializing {}", size,
            xdr_type_name<X>());
    size_t rem = size % 4;
    if (rem == 0) {
      if constexpr (Safe)
        need(size);
      x.resize_uninitialized(size);
      memcpy(x.data(), p, size);
      p += size;
    }
    else {
      std::size_t binlen = (size + 3UZ) & ~3UZ;
      if (Safe)
        need(binlen);
      x.resize_uninitialized(size);
      memcpy(x.data(), p, size);
      p += binlen;
      check_trailing_zeros(rem);
    }
  }

  bool unsafe_ok(SizeBounds b) const { return ee - p >= ptrdiff_t(b.max_); }

  template<bool Safe, is_xdr_cat<XdrCategory::kArray> X> void go(X &x)
  {
    if (!Safe || unsafe_ok(xdr_size_bounds<X>()))
      for (auto &e : x)
        go<false>(e);
    else
      for (auto &e : x)
        go<true>(e);
  }

  template<bool Safe, is_xdr_cat<XdrCategory::kVec> X> void go(X &x)
  {
    size_t size = get32<Safe>();
    x.resize(0);
    // Don't let bad input exhaust memory, but 64kB usually enough.
    x.reserve(
        std::min<uint32_t>(size, 0x1'0000 / sizeof(typename X::value_type)));
    if (!Safe ||
        unsafe_ok(xdr_size_bounds<typename X::value_type>().zero_to(size)))
      for (size_t i = 0; i < size; ++i) {
        x.resize_uninitialized(i + 1);
        go<false>(x[i]);
      }
    else
      for (size_t i = 0; i < size; ++i) {
        x.resize_uninitialized(i + 1);
        go<true>(x[i]);
      }
  }
  template<bool Safe, is_xdr_cat<XdrCategory::kPtr> X> void go(X &x)
  {
    bool b;
    go<Safe>(b);
    x.resize(b);
    if (b)
      go<Safe>(*x);
  }

  template<bool Safe, is_xdr_struct X> void go(X &x)
  {
    constexpr SizeBounds kSB = xdr_size_bounds<X>();
    if constexpr (CheckRecursion && is_recursive_xdr<X>)
      this->recurse();
    if constexpr (std::tuple_size_v<X> == 0)
      return;
    else if (!Safe || unsafe_ok(kSB))
      visit_fields([this](auto &x) { this->go<false>(x); }, x);
    else if constexpr (kSB.min_ <
                       xdr_size_bounds<std::tuple_element_t<0, X>>().max_)
      visit_fields([this](auto &x) { this->go<true>(x); }, x);
    // If we are Safe, but the minimum size of the whole structure is
    // greater than the maximum size of field 0, then we might as well
    // check once against the minimum size of the structure and then
    // unmarshal the first few fields unsafely.  That optimizes the
    // case of a structure that starts with a bunch of integers and
    // ends with a vector, like call_body.
    else if (ee - p < ptrdiff_t(xdr_size_bounds<X>().min_)) {
      std::println("input too short A");
      throw XdrBadInput("x23::Deserializer: input too short");
    }
    else {
      constexpr auto kUF = first_unsafe_field<X>(xdr_size_bounds<X>().min_);
      [this, &x]<size_t... I>(std::index_sequence<I...>) {
        (this->go<(I >= kUF)>(get<I>(x)), ...);
      }(std::make_index_sequence<std::tuple_size_v<X>>{});
    }
  }
  template<bool Safe, is_xdr_union X> void go(X &x)
  {
    if constexpr (CheckRecursion && is_recursive_xdr<X>)
      this->recurse();
    using ops = union_ops<X>;
    auto d = static_cast<typename ops::discriminant_type>(get32<Safe>());
    union_set_discriminant(x, d);
    if (ops::index(x) == -1) [[unlikely]]
      raise("Bad discriminant {} deserializing {}", get<0>(x),
            ops::meta::type_name);
    union_visit_arm([this](auto &x) { this->go<Safe>(x); }, x);
  }

  template<typename... Args>
  [[noreturn]] void raise(std::format_string<Args...> fmt, Args &&...args) const
  {
    // If we went beyond the buffer end, we will have read garbage, so
    // preferentially throw an input too short error.
    check();
    std::string msg = std::vformat(fmt.get(), std::make_format_args(args...));
    throw XdrBadInput{std::move(msg)};
  }
};

} // namespace detail

template<is_xdr_type Xout>
size_t
deserialize_from(Xout *out, BufViewEnd buf)
{
  detail::Deserializer<contains_recursive_xdr<Xout>> d(buf);
  d.template go<true>(*out);
  d.check();
  return d.p - buf.data();
}

template<is_xdr_type Xout>
inline void
deserialize(Xout *out, BufViewEnd buf)
{
  if (buf.size() % 4)
    throw XdrBadInput("input not multiple of 4 bytes deserializing {}",
                      xdr_type_name<Xout>());
  if (deserialize_from(out, buf) != buf.size())
    throw XdrBadInput("leftover bytes deserializing {}", xdr_type_name<Xout>());
}

} // namespace x23
