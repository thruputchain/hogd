#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include <x23/vec.h>
#include <x23/x23.h>

namespace x23 {

// De/serializer for data structures in CXS format.
// Attempt to deserialize an invalid binary Cxs representation.
struct CxsBadInput : XdrException {
  using XdrException::XdrException;
};

inline void check_size(std::span<const uint8_t> data, size_t required_size) {
    if (data.size() < required_size) {
        throw CxsBadInput("Not enough data to load Cxs value");
    }
}

constexpr static uint8_t kCxsHeader1Byte = 0b0011'1111; // 63
constexpr static uint8_t kCxsHeader2Byte = 0b0111'1111; // 127
constexpr static uint8_t kCxsHeader4Byte = 0b1011'1111; // 191
constexpr static uint8_t kCxsHeader8Byte = 0b1111'1111; // 255

constexpr static int8_t kCxsInt81ByteMin = -31; // inclusive
constexpr static int8_t kCxsInt81ByteMax = 31; // inclusive

constexpr static uint8_t kCxsUInt81ByteMax = 62; // inclusive

template<size_t N>
constexpr uint8_t lookup_header_byte() {
    if constexpr (N == 1) {
        return kCxsHeader1Byte;
    } else if constexpr (N == 2) {
        return kCxsHeader2Byte;
    } else if constexpr (N == 4) {
        return kCxsHeader4Byte;
    } else if constexpr (N == 8) {
        return kCxsHeader8Byte;
    } else {
        std::unreachable();
    }
}

template<size_t N>
constexpr uint64_t uint_canonical_upper_bound() {
    if constexpr (N == 0) {
        return kCxsUInt81ByteMax;
    } else if constexpr (N == 1) {
        return std::numeric_limits<uint8_t>::max();
    } else if constexpr (N == 2) {
        return std::numeric_limits<uint16_t>::max();
    } else if constexpr (N == 4) {
        return std::numeric_limits<uint32_t>::max();
    } else {
        std::unreachable();
    }
}

template<size_t N>
constexpr std::pair<int64_t, int64_t> int_canonical_bounds() {
    if constexpr (N == 0) {
        return {kCxsInt81ByteMin, kCxsInt81ByteMax};
    } else if constexpr (N == 1) {
        return {std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()};
    } else if constexpr (N == 2) {
        return {std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()};
    } else if constexpr (N == 4) {
        return {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()};
    } else {
        std::unreachable();
    }
}

template<std::integral T>
void load_canonical_check(T value) {
    constexpr size_t kSize = sizeof(T);
    if constexpr (std::is_unsigned_v<T>) {
        if (value <= uint_canonical_upper_bound<kSize/2>()) {
            throw CxsBadInput("Non canonical Cxs representation");
        }
    } else {
        if (value >= int_canonical_bounds<kSize/2>().first && value <= int_canonical_bounds<kSize/2>().second) {
            throw CxsBadInput("Non canonical Cxs representation");
        }
    }
}

template<std::integral U, std::integral T>
T load_cxs_integral_impl(std::span<const uint8_t>& data) {
    check_size(data, 1 + sizeof(U));
    if constexpr(sizeof(T) < sizeof(U)) {
        throw CxsBadInput("Non canonical Cxs representation");
    } else {
        U out;
        std::memcpy(&out, data.data() + 1, sizeof(U));
        data = data.subspan(sizeof(U) + 1);
        load_canonical_check<U>(out);
        return out;
    }
}

// Load a uint64_t from a span of bytes.
// Modifies the span to advance past the loaded value.
template<std::unsigned_integral T>
T load_cxs_uint(std::span<const uint8_t>& data) {
    check_size(data, 1);
    uint8_t first_byte = data[0];
    if (first_byte <= kCxsUInt81ByteMax) {
        data = data.subspan(1);
        return first_byte;
    }

    assert((first_byte & 0b0011'1111) == 63);
    uint8_t header = first_byte >> 6;

    constexpr std::array<T(*)(std::span<const uint8_t>&), 4> kCxsLoadUInt = {
        &load_cxs_integral_impl<uint8_t, T>,
        &load_cxs_integral_impl<uint16_t, T>,
        &load_cxs_integral_impl<uint32_t, T>,
        &load_cxs_integral_impl<uint64_t, T>,
    };
    assert(header < kCxsLoadUInt.size());
    return kCxsLoadUInt[header](data);
}

inline void store_cxs_int8_0_extra_byte(x23::Buf& buf, int64_t value) {
    assert(value >= kCxsInt81ByteMin && value <= kCxsInt81ByteMax);
    // use 1 byte representation
    if (value >= 0) {
        buf.push_back(value);
    } else {
        // invert and set highbit
        buf.push_back(((~value) & 0b0001'1111) | 0b0010'0000);
    }
}

template<size_t N> requires (N > 0)
void store_cxs_int_extra_bytes(x23::Buf& buf, int64_t value) {
    assert(value < int_canonical_bounds<N/2>().first || value > int_canonical_bounds<N/2>().second); // value is in canonical range for this size
    buf.push_back(lookup_header_byte<N>());
    buf.resize_uninitialized(buf.size() + N);
    std::memcpy(buf.data() + buf.size() - N, &value, N);
} 

constexpr static std::array<void(*)(x23::Buf&, int64_t), 5> kCxsStoreInt = {
    &store_cxs_int8_0_extra_byte,
    &store_cxs_int_extra_bytes<1>,
    &store_cxs_int_extra_bytes<2>,
    &store_cxs_int_extra_bytes<4>,
    &store_cxs_int_extra_bytes<8>,
};

template<std::signed_integral T>
uint8_t cxs_int_get_function_index(T value) {
    if (value >= int_canonical_bounds<0>().first && value <= int_canonical_bounds<0>().second) {
        return 0;
    }
    if (value >= int_canonical_bounds<1>().first && value <= int_canonical_bounds<1>().second) {
        return 1;
    }
    if (value >= int_canonical_bounds<2>().first && value <= int_canonical_bounds<2>().second) {
        return 2;
    }
    if (value >= int_canonical_bounds<4>().first && value <= int_canonical_bounds<4>().second) {
        return 3;
    }
    return 4;
}

inline void store_cxs_uint8_0_extra_byte(x23::Buf& buf, uint64_t value) {
    assert(value <= kCxsUInt81ByteMax);
    buf.push_back(value);
}

template<size_t N> requires (N > 0)
void store_cxs_uint_extra_bytes(x23::Buf& buf, uint64_t value) {
    assert(value > uint_canonical_upper_bound<N/2>()); // value is in canonical range for this size
    buf.push_back(lookup_header_byte<N>());
    buf.resize_uninitialized(buf.size() + N);
    std::memcpy(buf.data() + buf.size() - N, &value, N);
}

constexpr static std::array<void(*)(x23::Buf&, uint64_t), 5> kCxsStoreUInt = {
    &store_cxs_uint8_0_extra_byte,
    &store_cxs_uint_extra_bytes<1>,
    &store_cxs_uint_extra_bytes<2>,
    &store_cxs_uint_extra_bytes<4>,
    &store_cxs_uint_extra_bytes<8>,
};

template<std::unsigned_integral T>
uint8_t cxs_uint_get_function_index(T value) {
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
inline void store_cxs_uint(x23::Buf& buf, T value) {
    kCxsStoreUInt[cxs_uint_get_function_index(value)](buf, value);
}

template<std::signed_integral T>
inline void store_cxs_int(x23::Buf& buf, T value) {
    kCxsStoreInt[cxs_int_get_function_index(value)](buf, value);
}

inline void store_cxs_bool(x23::Buf& buf, bool value) {
    store_cxs_uint<uint8_t>(buf, value ? 1 : 0);
}

inline bool load_cxs_bool(std::span<const uint8_t>& data) {
    auto value = load_cxs_uint<uint8_t>(data);
    if (value != 0 && value != 1) {
        throw CxsBadInput("Non canonical Cxs representation");
    }
    return value != 0;
}

template<std::signed_integral T>
T load_cxs_int(std::span<const uint8_t>& data) {
    check_size(data, 1);
    uint8_t first_byte = data[0];
    if (first_byte <= kCxsUInt81ByteMax) {
        data = data.subspan(1);
        // convert to signed
        if (first_byte & 0b0010'0000) {
            return ~(first_byte & 0b0001'1111);
        }
        return first_byte;
    }
    assert((first_byte & 0b0011'1111) == 63);
    uint8_t header = first_byte >> 6;

    constexpr std::array<T(*)(std::span<const uint8_t>&), 4> kCxsLoadInt = {
        &load_cxs_integral_impl<int8_t, T>,
        &load_cxs_integral_impl<int16_t, T>,
        &load_cxs_integral_impl<int32_t, T>,
        &load_cxs_integral_impl<int64_t, T>,
    };
    assert(header < kCxsLoadInt.size());
    return kCxsLoadInt[header](data);
}

template<typename T>
struct CxsSerializer;

template<std::integral T>
struct CxsSerializer<T> {
    static void store(x23::Buf& buf, T value) {
        if constexpr (std::is_same_v<T, bool>) {
            store_cxs_bool(buf, value);
        } else if constexpr (std::signed_integral<T>) {
            store_cxs_int<T>(buf, value);
        } else {
            store_cxs_uint<T>(buf, value);
        }
    }
    static void load(std::span<const uint8_t>& data, T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            value = load_cxs_bool(data);
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            value = load_cxs_int<T>(data);
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            value = load_cxs_uint<T>(data);
        } else {
            static_assert(false, "impossible");
        }
    }
};

template<typename T> requires (std::is_enum_v<T>)
struct CxsSerializer<T> {
    static_assert(std::is_integral_v<std::underlying_type_t<T>>);
    static void store(x23::Buf& buf, T value) {
        CxsSerializer<std::underlying_type_t<T>>::store(buf, static_cast<std::underlying_type_t<T>>(value));
    }
    static void load(std::span<const uint8_t>& data, T& value) {
        std::underlying_type_t<T> underlying_value;
        CxsSerializer<std::underlying_type_t<T>>::load(data, underlying_value);
        value = static_cast<T>(underlying_value);
    }
};

template<is_xdr_struct T>
struct CxsSerializer<T> {
    static void store(x23::Buf& buf, const T& value) {
        [&]<size_t... I>(std::index_sequence<I...>) {
            (CxsSerializer<std::tuple_element_t<I, T>>::store(buf, get<I>(value)), ...);
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});
    }
    static void load(std::span<const uint8_t>& data, T& value) {
        [&]<size_t... I>(std::index_sequence<I...>) {
            ((CxsSerializer<std::tuple_element_t<I, T>>::load(data, get<I>(value))), ...);
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});
    }
};

template<is_xdr_union T>
struct CxsSerializer<T> {
    static void store(x23::Buf& buf, const T& value) {
        if (!union_valid(value)) {
            throw XdrBadUnionAccess(
                "Attempt to serialize {} with invalid discrminant {}",
                xdr_meta<T>::type_name, get<0>(value));
        }
        using discriminant_type = typename union_ops<T>::discriminant_type;
        // store the discriminant
        CxsSerializer<discriminant_type>::store(buf, get<0>(value));

        union_visit_arm([&](auto&& arm) {
            CxsSerializer<std::remove_cvref_t<decltype(arm)>>::store(buf, arm);
        }, value);
    }

    static void load(std::span<const uint8_t>& data, T& value) {
        using ops = union_ops<T>;
        using discriminant_type = typename ops::discriminant_type;
        discriminant_type d;
        CxsSerializer<discriminant_type>::load(data, d);
        union_set_discriminant(value, d);
        if (ops::index(value) == -1) {
            throw CxsBadInput("Bad discriminant {} deserializing {}", get<0>(value),
                        ops::meta::type_name);
        }
        union_visit_arm([&](auto &x) { CxsSerializer<std::remove_cvref_t<decltype(x)>>::load(data, x); }, value);
    }
};

template<is_xdr_cat<XdrCategory::kArray> X>
struct CxsSerializer<X> {
    static void store(x23::Buf& buf, const X& value) {
        for (const auto& element : value) {
            CxsSerializer<typename X::value_type>::store(buf, element);
        }
    }
    static void load(std::span<const uint8_t>& data, X& value) {
        for (auto& element : value) {
            CxsSerializer<typename X::value_type>::load(data, element);
        }
    }
};

template<is_xdr_cat<XdrCategory::kVec> X>
struct CxsSerializer<X> {
    static void store(x23::Buf& buf, const X& value) {
        CxsSerializer<uint32_t>::store(buf, value.size());
        for (const auto& element : value) {
            CxsSerializer<typename X::value_type>::store(buf, element);
        }
    }
    static void load(std::span<const uint8_t>& data, X& value) {
        uint32_t size;
        CxsSerializer<uint32_t>::load(data, size);
        // Don't let bad input exhaust memory, but 64kB usually enough.
        value.reserve(
            std::min<std::uint32_t>(size,
                                    0x10000 / sizeof(typename X::value_type)));
        for (size_t i = 0; i < size; ++i) {
            value.resize_uninitialized(i + 1);
            CxsSerializer<typename X::value_type>::load(data, value[i]);
        }
    }
};

template<is_xdr_cat<XdrCategory::kOpaqueVec, XdrCategory::kString> X>
struct CxsSerializer<X> {
    static void store(x23::Buf& buf, const X& value) {
        CxsSerializer<uint32_t>::store(buf, value.size());
        buf.append_range(value);
    }
    static void load(std::span<const uint8_t>& data, X& value) {
        uint32_t size;
        CxsSerializer<uint32_t>::load(data, size);
        if (size > X::max_size()) [[unlikely]]
            throw CxsBadInput("length {} exceeds maximum deserializing {}", size,
                        xdr_type_name<X>());
        value.resize_uninitialized(size);
        std::memcpy(value.data(), data.data(), size);
        data = data.subspan(size);
    }
};

template<is_xdr_cat<XdrCategory::kOpaqueArray> X>
struct CxsSerializer<X> {
    static void store(x23::Buf& buf, const X& value) {
        buf.append_range(value);
    }
    static void load(std::span<const uint8_t>& data, X& value) {
        check_size(data, value.size());
        std::memcpy(value.data(), data.data(), value.size());
        data = data.subspan(value.size());
    }
};

template<is_xdr_cat<XdrCategory::kPtr> X>
struct CxsSerializer<X> {
    static void store(x23::Buf& buf, const X& value) {
        bool b = value.get() != nullptr;
        CxsSerializer<bool>::store(buf, b);
        if (b) {
            CxsSerializer<typename X::value_type>::store(buf, *value);
        }
    }
       
    static void load(std::span<const uint8_t>& data, X& value) {
        bool b;
        CxsSerializer<bool>::load(data, b);
        value.resize(b);
        if (b) {
            CxsSerializer<typename X::value_type>::load(data, *value);
        }
    }
};

// float and double are special cased because they are not integral types
template<typename T> requires (is_xdr_cat<T, XdrCategory::kPrimitive> && !std::is_integral_v<T>)
struct CxsSerializer<T> {
    using uint_type = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    static_assert(sizeof(uint_type) == sizeof(T), "uint_type must be the same size as T");

    static void store(x23::Buf& buf, T value) {
        CxsSerializer<uint_type>::store(buf, std::bit_cast<uint_type>(value));
    }
    static void load(std::span<const uint8_t>& data, T& value) {
        uint_type bits;
        CxsSerializer<uint_type>::load(data, bits);
        value = std::bit_cast<T>(bits);
    }
};

inline void serialize_cxs(x23::Buf& buf, const is_xdr_type auto& value) {
    CxsSerializer<std::remove_cvref_t<decltype(value)>>::store(buf, value);
}
template<is_xdr_type T>
void deserialize_cxs(std::span<const uint8_t>& data, T& value) {
    CxsSerializer<std::remove_cvref_t<decltype(value)>>::load(data, value);
    if (data.size() != 0) {
        throw CxsBadInput("Extra data after deserializing {}", xdr_type_name<T>());
    }
}

} // namespace x23
