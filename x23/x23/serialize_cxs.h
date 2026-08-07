#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <x23/debug.h>
#include <x23/err.h>
#include <x23/serialize.h>
#include <x23/utility.h>
#include <x23/vec.h>
#include <x23/x23.h>

namespace x23 {

static_assert(std::endian::native == std::endian::little,
              "CXS serialization uses native-endian memcpy for integers "
              "and is only correct on little-endian platforms");

enum class CxsErrorCode {
  kTruncated = 1,
  kNonCanonical,
  kBadDiscriminant,
  kLengthExceeded,
  kRecursionLimit,
  kTrailingData,
};

class CxsErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "cxs"; }
  std::string message(int code) const override;
};

struct CxsError : public ErrorBase {
  constexpr explicit CxsError(CxsErrorCode code) : code_(code) {}

  constexpr CxsErrorCode code() const { return code_; }
  constexpr int raw_code() const
  {
    return static_cast<int>(std::to_underlying(code_));
  }
  static const std::error_category &category()
  {
    static const CxsErrorCategory cat;
    return cat;
  }

private:
  CxsErrorCode code_;
};

constexpr bool
operator==(const CxsError &lhs, CxsErrorCode rhs)
{
  return lhs.code() == rhs;
}

inline bool
operator==(const CxsError &lhs, const std::error_code &rhs)
{
  // NOLINTNEXTLINE(readability-static-accessed-through-instance)
  return lhs.category() == rhs.category() && lhs.raw_code() == rhs.value();
}

inline bool
operator==(const CxsErrorCode &lhs, const std::error_code &rhs)
{
  return CxsError{lhs} == rhs;
}

template<typename T> using CxsStatus = std::expected<T, CxsError>;

inline CxsStatus<void>
check_size(std::span<const uint8_t> data, size_t required_size)
{
  if (data.size() < required_size) {
    return CxsError{CxsErrorCode::kTruncated};
  }
  return {};
}

constexpr static uint8_t kCxsHeader1Byte = 0b0011'1111; // 63
constexpr static uint8_t kCxsHeader2Byte = 0b0111'1111; // 127
constexpr static uint8_t kCxsHeader4Byte = 0b1011'1111; // 191
constexpr static uint8_t kCxsHeader8Byte = 0b1111'1111; // 255

constexpr static int8_t kCxsInt81ByteMin = -31; // inclusive
constexpr static int8_t kCxsInt81ByteMax = 31;  // inclusive

constexpr static uint8_t kCxsUInt81ByteMax = 62; // inclusive

template<size_t N>
constexpr uint8_t
lookup_header_byte()
{
  if constexpr (N == 1) {
    return kCxsHeader1Byte;
  }
  else if constexpr (N == 2) {
    return kCxsHeader2Byte;
  }
  else if constexpr (N == 4) {
    return kCxsHeader4Byte;
  }
  else if constexpr (N == 8) {
    return kCxsHeader8Byte;
  }
  else {
    std::unreachable();
  }
}

template<size_t N>
constexpr uint64_t
uint_canonical_upper_bound()
{
  if constexpr (N == 0) {
    return kCxsUInt81ByteMax;
  }
  else if constexpr (N == 1) {
    return std::numeric_limits<uint8_t>::max();
  }
  else if constexpr (N == 2) {
    return std::numeric_limits<uint16_t>::max();
  }
  else if constexpr (N == 4) {
    return std::numeric_limits<uint32_t>::max();
  }
  else {
    std::unreachable();
  }
}

template<size_t N>
constexpr std::pair<int64_t, int64_t>
int_canonical_bounds()
{
  if constexpr (N == 0) {
    return {kCxsInt81ByteMin, kCxsInt81ByteMax};
  }
  else if constexpr (N == 1) {
    return {std::numeric_limits<int8_t>::min(),
            std::numeric_limits<int8_t>::max()};
  }
  else if constexpr (N == 2) {
    return {std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max()};
  }
  else if constexpr (N == 4) {
    return {std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()};
  }
  else {
    std::unreachable();
  }
}

template<std::integral T>
CxsStatus<void>
load_canonical_check(T value)
{
  constexpr size_t kSize = sizeof(T);
  if constexpr (std::is_unsigned_v<T>) {
    if (value <= uint_canonical_upper_bound<kSize / 2>()) {
      return CxsError{CxsErrorCode::kNonCanonical};
    }
  }
  else {
    if (value >= int_canonical_bounds<kSize / 2>().first &&
        value <= int_canonical_bounds<kSize / 2>().second) {
      return CxsError{CxsErrorCode::kNonCanonical};
    }
  }
  return {};
}

template<std::integral U, std::integral T>
CxsStatus<T>
load_cxs_integral_impl(std::span<const uint8_t> &data)
{
  if (auto status = check_size(data, 1 + sizeof(U)); !status.has_value())
      [[unlikely]] {
    return status.error();
  }
  if constexpr (sizeof(T) < sizeof(U)) {
    return CxsError{CxsErrorCode::kNonCanonical};
  }
  else {
    U out;
    std::memcpy(&out, data.data() + 1, sizeof(U));
    data = data.subspan(sizeof(U) + 1);
    if (auto status = load_canonical_check<U>(out); !status.has_value())
        [[unlikely]] {
      return status.error();
    }
    return out;
  }
}

// Load a uint64_t from a span of bytes.
// Modifies the span to advance past the loaded value.
template<std::unsigned_integral T>
CxsStatus<T>
load_cxs_uint(std::span<const uint8_t> &data)
{
  if (auto status = check_size(data, 1); !status.has_value()) [[unlikely]] {
    return status.error();
  }
  uint8_t first_byte = data[0];
  if (first_byte <= kCxsUInt81ByteMax) {
    data = data.subspan(1);
    return first_byte;
  }

  uint8_t lowbits = first_byte & 0b0011'1111;
  if (lowbits != 63) {
    return CxsError{CxsErrorCode::kNonCanonical};
  }

  uint8_t header = first_byte >> 6;
  switch (header) {
  case 0:
    return load_cxs_integral_impl<uint8_t, T>(data);
  case 1:
    return load_cxs_integral_impl<uint16_t, T>(data);
  case 2:
    return load_cxs_integral_impl<uint32_t, T>(data);
  case 3:
    return load_cxs_integral_impl<uint64_t, T>(data);
  default:
    std::unreachable();
  }
}

constexpr void
store_cxs_int8_0_extra_byte(x23::Buf &buf, int64_t value)
{
  x23assert(value >= kCxsInt81ByteMin && value <= kCxsInt81ByteMax);
  // use 1 byte representation
  if (value >= 0) {
    buf.push_back(value);
  }
  else {
    // invert and set highbit
    int8_t inverted = -value;
    buf.push_back(((~inverted) & 0b0001'1111) | 0b0010'0000);
  }
}

template<size_t N> requires (N > 0)
constexpr void
store_cxs_int_extra_bytes(x23::Buf &buf, int64_t value)
{
  x23assert(value < int_canonical_bounds<N / 2>().first ||
            value > int_canonical_bounds<N / 2>()
                        .second); // value is in canonical range for this size
  size_t offset = buf.size();
  buf.resize_uninitialized(offset + 1 + N);
  buf[offset] = lookup_header_byte<N>();
  if consteval {
    auto bytes = std::bit_cast<std::array<uint8_t, sizeof(value)>>(value);
    for (size_t i = 0; i < N; ++i)
      buf[offset + 1 + i] = bytes[i];
  }
  else {
    std::memcpy(buf.data() + offset + 1, &value, N);
  }
}

template<std::signed_integral T>
constexpr uint8_t
cxs_int_get_function_index(T value)
{
  if (value >= int_canonical_bounds<0>().first &&
      value <= int_canonical_bounds<0>().second) {
    return 0;
  }
  if (value >= int_canonical_bounds<1>().first &&
      value <= int_canonical_bounds<1>().second) {
    return 1;
  }
  if (value >= int_canonical_bounds<2>().first &&
      value <= int_canonical_bounds<2>().second) {
    return 2;
  }
  if (value >= int_canonical_bounds<4>().first &&
      value <= int_canonical_bounds<4>().second) {
    return 3;
  }
  return 4;
}

constexpr inline void
store_cxs_uint8_0_extra_byte(x23::Buf &buf, uint64_t value)
{
  x23assert(value <= kCxsUInt81ByteMax);
  buf.push_back(value);
}

template<size_t N> requires (N > 0)
constexpr void
store_cxs_uint_extra_bytes(x23::Buf &buf, uint64_t value)
{
  x23assert(value >
            uint_canonical_upper_bound<N / 2>()); // value is in canonical
                                                  // range for this size
  size_t offset = buf.size();
  buf.resize_uninitialized(offset + 1 + N);
  buf[offset] = lookup_header_byte<N>();
  if consteval {
    auto bytes = std::bit_cast<std::array<uint8_t, sizeof(value)>>(value);
    for (size_t i = 0; i < N; ++i)
      buf[offset + 1 + i] = bytes[i];
  }
  else {
    std::memcpy(buf.data() + offset + 1, &value, N);
  }
}

template<std::unsigned_integral T>
constexpr uint8_t
cxs_uint_get_function_index(T value)
{
  if (value <= kCxsUInt81ByteMax) {
    return 0;
  }
  if (value <= std::numeric_limits<uint8_t>::max()) {
    return 1;
  }
  if (value <= std::numeric_limits<uint16_t>::max()) {
    return 2;
  }
  if (value <= std::numeric_limits<uint32_t>::max()) {
    return 3;
  }
  return 4;
}

template<std::unsigned_integral T>
constexpr inline void
store_cxs_uint(x23::Buf &buf, T value)
{
  switch (cxs_uint_get_function_index(value)) {
  case 0:
    return store_cxs_uint8_0_extra_byte(buf, value);
  case 1:
    return store_cxs_uint_extra_bytes<1>(buf, value);
  case 2:
    return store_cxs_uint_extra_bytes<2>(buf, value);
  case 3:
    return store_cxs_uint_extra_bytes<4>(buf, value);
  case 4:
    return store_cxs_uint_extra_bytes<8>(buf, value);
  default:
    std::unreachable();
  }
}

template<std::signed_integral T>
constexpr inline void
store_cxs_int(x23::Buf &buf, T value)
{
  switch (cxs_int_get_function_index(value)) {
  case 0:
    return store_cxs_int8_0_extra_byte(buf, value);
  case 1:
    return store_cxs_int_extra_bytes<1>(buf, value);
  case 2:
    return store_cxs_int_extra_bytes<2>(buf, value);
  case 3:
    return store_cxs_int_extra_bytes<4>(buf, value);
  case 4:
    return store_cxs_int_extra_bytes<8>(buf, value);
  default:
    std::unreachable();
  }
}

constexpr inline void
store_cxs_bool(x23::Buf &buf, bool value)
{
  store_cxs_uint<uint8_t>(buf, value ? 1 : 0);
}

inline CxsStatus<bool>
load_cxs_bool(std::span<const uint8_t> &data)
{
  auto value = load_cxs_uint<uint8_t>(data);
  if (!value.has_value()) [[unlikely]] {
    return value.error();
  }
  if (value != 0 && value != 1) {
    return CxsError{CxsErrorCode::kNonCanonical};
  }
  return value != 0;
}

template<std::signed_integral T>
CxsStatus<T>
load_cxs_int(std::span<const uint8_t> &data)
{
  if (auto status = check_size(data, 1); !status.has_value()) [[unlikely]] {
    return status.error();
  }
  uint8_t first_byte = data[0];
  if (first_byte <= kCxsUInt81ByteMax) {
    data = data.subspan(1);
    if (first_byte <= kCxsInt81ByteMax) {
      x23assert(!(first_byte & 0b0010'0000));
      return first_byte;
    }
    int8_t signed_byte = first_byte;
    return -(((~signed_byte) & 0b0001'1111));
  }
  if ((first_byte & 0b0011'1111) != 63) {
    return CxsError{CxsErrorCode::kNonCanonical};
  }
  uint8_t header = first_byte >> 6;
  switch (header) {
  case 0:
    return load_cxs_integral_impl<int8_t, T>(data);
  case 1:
    return load_cxs_integral_impl<int16_t, T>(data);
  case 2:
    return load_cxs_integral_impl<int32_t, T>(data);
  case 3:
    return load_cxs_integral_impl<int64_t, T>(data);
  default:
    std::unreachable();
  }
}

template<typename T> struct CxsSerializer;

template<std::integral T> struct CxsSerializer<T> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, T value)
  {
    if constexpr (std::is_same_v<T, bool>) {
      store_cxs_bool(buf, value);
    }
    else if constexpr (std::signed_integral<T>) {
      store_cxs_int<T>(buf, value);
    }
    else {
      store_cxs_uint<T>(buf, value);
    }
    return {};
  }
};

// Open-enum semantics: enum values outside `xdr_meta<T>::tags` are valid
// after decode. The unchecked static_cast below is intentional; consumers
// must handle unknown tags defensively. Do not add tag-set validation here.
template<typename T> requires (std::is_enum_v<T>)
struct CxsSerializer<T> {
  static_assert(std::is_integral_v<std::underlying_type_t<T>>);
  static constexpr CxsStatus<void> store(x23::Buf &buf, T value)
  {
    return CxsSerializer<std::underlying_type_t<T>>::store(
        buf, static_cast<std::underlying_type_t<T>>(value));
  }
};

template<is_xdr_struct T> struct CxsSerializer<T> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const T &value)
  {
    CxsStatus<void> status;
    [&]<size_t... I>(std::index_sequence<I...>) {
      (
          [&] {
            if (status)
              status = CxsSerializer<std::tuple_element_t<I, T>>::store(
                  buf, get<I>(value));
          }(),
          ...);
    }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});
    return status;
  }
};

template<is_xdr_union T> struct CxsSerializer<T> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const T &value)
  {
    if (!union_valid(value)) [[unlikely]]
      return CxsError{CxsErrorCode::kBadDiscriminant};
    using discriminant_type = typename union_ops<T>::discriminant_type;
    if (auto status =
            CxsSerializer<discriminant_type>::store(buf, get<0>(value));
        !status) [[unlikely]]
      return status.error();

    return union_visit_arm(
        [&](auto &&arm) {
          return CxsSerializer<std::remove_cvref_t<decltype(arm)>>::store(buf,
                                                                          arm);
        },
        value, CxsStatus<void>{});
  }
};

template<is_xdr_cat<XdrCategory::kArray> X> struct CxsSerializer<X> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const X &value)
  {
    for (const auto &element : value) {
      if (auto status =
              CxsSerializer<typename X::value_type>::store(buf, element);
          !status) [[unlikely]]
        return status.error();
    }
    return {};
  }
};

template<is_xdr_cat<XdrCategory::kVec> X> struct CxsSerializer<X> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const X &value)
  {
    if (auto status = CxsSerializer<uint32_t>::store(buf, value.size());
        !status) [[unlikely]]
      return status.error();
    for (const auto &element : value) {
      if (auto status =
              CxsSerializer<typename X::value_type>::store(buf, element);
          !status) [[unlikely]]
        return status.error();
    }
    return {};
  }
};

template<is_xdr_cat<XdrCategory::kOpaqueVec, XdrCategory::kString> X>
struct CxsSerializer<X> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const X &value)
  {
    if (auto status = CxsSerializer<uint32_t>::store(buf, value.size());
        !status) [[unlikely]]
      return status.error();
    buf.append_range(value);
    return {};
  }
};

template<> struct CxsSerializer<x23::BufView> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, x23::BufView value)
  {
    if (auto status = CxsSerializer<uint32_t>::store(buf, value.size());
        !status) [[unlikely]]
      return status.error();
    buf.append_range(value);
    return {};
  }
};

template<is_xdr_cat<XdrCategory::kOpaqueArray> X> struct CxsSerializer<X> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const X &value)
  {
    buf.append_range(value);
    return {};
  }
};

template<is_xdr_cat<XdrCategory::kPtr> X> struct CxsSerializer<X> {
  static constexpr CxsStatus<void> store(x23::Buf &buf, const X &value)
  {
    bool b = value.get() != nullptr;
    if (auto status = CxsSerializer<bool>::store(buf, b); !status) [[unlikely]]
      return status.error();
    if (b) {
      return CxsSerializer<typename X::value_type>::store(buf, *value);
    }
    return {};
  }
};

// float and double are special cased because they are not integral types
template<typename T>
requires (is_xdr_cat<T, XdrCategory::kPrimitive> && !std::is_integral_v<T>)
struct CxsSerializer<T> {
  using uint_type = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
  static_assert(sizeof(uint_type) == sizeof(T),
                "uint_type must be the same size as T");

  static constexpr CxsStatus<void> store(x23::Buf &buf, T value)
  {
    return CxsSerializer<uint_type>::store(buf,
                                           std::bit_cast<uint_type>(value));
  }
};

[[nodiscard]] constexpr CxsStatus<void>
serialize_cxs_to(x23::Buf &buf, const is_xdr_type auto &value)
{
  return CxsSerializer<std::remove_cvref_t<decltype(value)>>::store(buf, value);
}

[[nodiscard]] constexpr CxsStatus<x23::Buf>
serialize_cxs(const is_xdr_type auto &...value)
{
  x23::Buf buf;
  CxsStatus<void> status;
  (
      [&] {
        if (status)
          status = serialize_cxs_to(buf, value);
      }(),
      ...);
  if (!status) [[unlikely]]
    return status.error();
  return buf;
}

// Encoded byte length of a CXS integer (header + payload).
constexpr size_t
cxs_integral_encoded_size(uint8_t function_index) noexcept
{
  switch (function_index) {
  case 0:
    return 1;
  case 1:
    return 2;
  case 2:
    return 3;
  case 3:
    return 5;
  case 4:
    return 9;
  default:
    std::unreachable();
  }
}

constexpr size_t
cxs_size(std::monostate /*unused*/) noexcept
{
  return 0;
}

// Exact CXS byte length of a concrete value (value-dependent, constexpr).
template<typename T>
constexpr size_t
cxs_size(const T &x)
{
  using X = std::remove_cvref_t<T>;
  using enum XdrCategory;
  if constexpr (std::same_as<X, bool>)
    return 1;
  else if constexpr (std::signed_integral<X>)
    return cxs_integral_encoded_size(cxs_int_get_function_index(x));
  else if constexpr (std::unsigned_integral<X>)
    return cxs_integral_encoded_size(cxs_uint_get_function_index(x));
  else if constexpr (std::is_enum_v<X>)
    return cxs_size(static_cast<std::underlying_type_t<X>>(x));
  else if constexpr (std::same_as<X, float> || std::same_as<X, double>) {
    using uint_type = std::conditional_t<sizeof(X) == 4, uint32_t, uint64_t>;
    return cxs_size(std::bit_cast<uint_type>(x));
  }
  else if constexpr (is_xdr_cat<X, kOpaqueArray>)
    return std::tuple_size_v<X>;
  else if constexpr (is_xdr_cat<X, kOpaqueVec, kString>)
    return cxs_size(static_cast<uint32_t>(x.size())) + x.size();
  else if constexpr (is_xdr_cat<X, kArray>) {
    size_t size = 0;
    for (const auto &e : x)
      size += cxs_size(e);
    return size;
  }
  else if constexpr (is_xdr_cat<X, kVec>) {
    size_t size = cxs_size(static_cast<uint32_t>(x.size()));
    for (const auto &e : x)
      size += cxs_size(e);
    return size;
  }
  else if constexpr (is_xdr_cat<X, kPtr>)
    return cxs_size(bool{x.get() != nullptr}) + (x.get() ? cxs_size(*x) : 0UZ);
  else if constexpr (is_xdr_union<X>)
    return cxs_size(get<0>(x)) +
           union_visit_arm([](const auto &a) { return cxs_size(a); }, x, 0UZ);
  else if constexpr (is_xdr_struct<X>) {
    size_t size = 0;
    visit_fields([&size](const auto &f) { size += cxs_size(f); }, x);
    return size;
  }
  else
    std::unreachable();
}

// Serialize a concrete value into an array sized to its exact CXS encoding.
// N must equal cxs_size(value); call as
// serialize_cxs_array<cxs_size(obj)>(obj).
namespace detail {
[[noreturn]] inline void
cxs_array_serialization_failed()
{
  std::abort();
}

[[noreturn]] inline void
cxs_array_size_mismatch()
{
  std::abort();
}
} // namespace detail

template<size_t N, is_xdr_type T>
consteval std::array<std::uint8_t, N>
serialize_cxs_array(const T &value)
{
  x23::Buf buf;
  if (auto status = serialize_cxs_to(buf, value); !status) [[unlikely]]
    detail::cxs_array_serialization_failed();
  if (buf.size() != N) [[unlikely]]
    detail::cxs_array_size_mismatch();
  std::array<std::uint8_t, N> out{};
  for (size_t i = 0; i < N; ++i)
    out[i] = buf[i];
  return out;
}

template<bool CheckRecursion> struct CxsRecursionTracker {};

template<> struct CxsRecursionTracker<true> {
  size_t remaining_ = deserialization_recursion_limit;

  CxsStatus<void> recurse()
  {
    if (remaining_ == 0) [[unlikely]]
      return CxsError{CxsErrorCode::kRecursionLimit};
    --remaining_;
    return {};
  }
};

template<bool CheckRecursion>
struct CxsDeserializer : private CxsRecursionTracker<CheckRecursion> {
  std::span<const uint8_t> &data;

  explicit CxsDeserializer(std::span<const uint8_t> &d) noexcept : data(d) {}

  CxsStatus<void> go(std::monostate & /*unused*/) { return {}; }

  template<std::integral T> CxsStatus<void> go(T &value)
  {
    CxsStatus<T> status = [&] {
      if constexpr (std::same_as<T, bool>)
        return load_cxs_bool(data);
      else if constexpr (std::signed_integral<T>)
        return load_cxs_int<T>(data);
      else
        return load_cxs_uint<T>(data);
    }();
    if (!status) [[unlikely]]
      return status.error();
    value = *status;
    return {};
  }

  template<typename T> requires (std::is_enum_v<T>)
  CxsStatus<void> go(T &value)
  {
    std::underlying_type_t<T> underlying_value;
    if (auto status = go(underlying_value); !status) [[unlikely]]
      return status.error();
    value = static_cast<T>(underlying_value);
    return {};
  }

  template<typename T>
  requires (is_xdr_cat<T, XdrCategory::kPrimitive> && !std::is_integral_v<T>)
  CxsStatus<void> go(T &value)
  {
    using uint_type =
        std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t, uint64_t>;
    static_assert(sizeof(uint_type) == sizeof(T));
    uint_type bits;
    if (auto status = go(bits); !status) [[unlikely]]
      return status.error();
    value = std::bit_cast<T>(bits);
    return {};
  }

  template<is_xdr_cat<XdrCategory::kOpaqueVec, XdrCategory::kString> X>
  CxsStatus<void> go(X &value)
  {
    uint32_t size;
    if (auto status = go(size); !status) [[unlikely]]
      return status.error();
    if (size > X::max_size()) [[unlikely]]
      return CxsError{CxsErrorCode::kLengthExceeded};
    if (auto status = check_size(data, size); !status) [[unlikely]]
      return status.error();
    value.resize_uninitialized(size);
    if (size > 0)
      std::memcpy(value.data(), data.data(), size);
    data = data.subspan(size);
    return {};
  }

  CxsStatus<void> go(x23::BufView &value)
  {
    uint32_t size;
    if (auto status = go(size); !status) [[unlikely]]
      return status.error();
    if (auto status = check_size(data, size); !status) [[unlikely]]
      return status.error();
    value = x23::BufView(data.data(), size);
    data = data.subspan(size);
    return {};
  }

  template<is_xdr_cat<XdrCategory::kOpaqueArray> X> CxsStatus<void> go(X &value)
  {
    if (auto status = check_size(data, value.size()); !status) [[unlikely]]
      return status.error();
    if (!value.empty())
      std::memcpy(value.data(), data.data(), value.size());
    data = data.subspan(value.size());
    return {};
  }

  template<is_xdr_struct T> CxsStatus<void> go(T &value)
  {
    if constexpr (CheckRecursion && is_recursive_xdr<T>) {
      if (auto status = this->recurse(); !status) [[unlikely]]
        return status.error();
    }
    CxsStatus<void> status;
    visit_fields(
        [this, &status](auto &field) {
          if (status)
            status = this->go(field);
        },
        value);
    return status;
  }

  template<is_xdr_union T> CxsStatus<void> go(T &value)
  {
    if constexpr (CheckRecursion && is_recursive_xdr<T>) {
      if (auto status = this->recurse(); !status) [[unlikely]]
        return status.error();
    }
    using ops = union_ops<T>;
    using discriminant_type = typename ops::discriminant_type;
    discriminant_type d;
    if (auto status = go(d); !status) [[unlikely]]
      return status.error();
    union_set_discriminant(value, d);
    if (ops::index(value) == -1) [[unlikely]]
      return CxsError{CxsErrorCode::kBadDiscriminant};
    return union_visit_arm([this](auto &arm) { return this->go(arm); }, value,
                           CxsStatus<void>{});
  }

  template<is_xdr_cat<XdrCategory::kArray> X> CxsStatus<void> go(X &value)
  {
    for (auto &element : value) {
      if (auto status = go(element); !status) [[unlikely]]
        return status.error();
    }
    return {};
  }

  template<is_xdr_cat<XdrCategory::kVec> X> CxsStatus<void> go(X &value)
  {
    uint32_t size;
    if (auto status = go(size); !status) [[unlikely]]
      return status.error();
    if (size > X::max_size()) [[unlikely]]
      return CxsError{CxsErrorCode::kLengthExceeded};
    if constexpr (detail::xdr_size_bounds<typename X::value_type>().max_ == 0)
      // A zero-wire-size element consumes no input, so the count is not bounded
      // by the remaining input and (for an unbounded vector) not by max_size
      // above either; left unchecked it grows the vector until std::bad_alloc.
      // Cap materialization at the 64 KiB reserve budget used just below, the
      // same limit the XDR path applies in Deserializer::go.
      if (size > 0x10000 / sizeof(typename X::value_type)) [[unlikely]]
        return CxsError{CxsErrorCode::kLengthExceeded};
    value.resize(0);
    value.reserve(
        std::min<std::uint32_t>(size,
                                0x10000 / sizeof(typename X::value_type)));
    for (size_t i = 0; i < size; ++i) {
      value.resize_uninitialized(i + 1);
      if (auto status = go(value[i]); !status) [[unlikely]]
        return status.error();
    }
    return {};
  }

  template<is_xdr_cat<XdrCategory::kPtr> X> CxsStatus<void> go(X &value)
  {
    bool b;
    if (auto status = go(b); !status) [[unlikely]]
      return status.error();
    value.resize(b);
    if (b) {
      if (auto status = go(*value); !status) [[unlikely]]
        return status.error();
    }
    return {};
  }
};

template<is_xdr_type T>
[[nodiscard]] CxsStatus<size_t>
deserialize_cxs_from(T *value, std::span<const uint8_t> data)
{
  size_t initial_size = data.size();
  CxsDeserializer<contains_recursive_xdr<T>> d(data);
  if (auto status = d.go(*value); !status) [[unlikely]]
    return status.error();
  return initial_size - data.size();
}

template<is_xdr_type T>
[[nodiscard]] CxsStatus<void>
deserialize_cxs(T *value, std::span<const uint8_t> data)
{
  auto used = deserialize_cxs_from(value, data);
  if (!used) [[unlikely]]
    return used.error();
  if (*used != data.size()) [[unlikely]]
    return CxsError{CxsErrorCode::kTrailingData};
  return {};
}

} // namespace x23
