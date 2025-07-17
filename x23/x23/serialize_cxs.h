#pragma once

#include "x23/vec.h"
#include "x23/x23.h"
#include <algorithm>
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

// Load a uint64_t from a span of bytes.
// Modifies the span to advance past the loaded value.
template<std::unsigned_integral T>
T load_cxs_unsigned_integral(std::span<const uint8_t>& data) {
    check_size(data, 1);
    uint8_t first_byte = data[0];
    if (first_byte <= kCxsUInt81ByteMax) {
        data = data.subspan(1);
        return first_byte;
    }
    assert((first_byte & 0b0011'1111) == 63);
    uint8_t header = first_byte >> 6;
    switch(header) {
        case 0:
        {
            check_size(data, 2);
            uint8_t out = data[1];
            data = data.subspan(2);
            if (out <= kCxsUInt81ByteMax) {
                throw CxsBadInput("Non canonical Cxs representation");
            }
            return out;
        }
        case 1: {
            if constexpr (sizeof(T) < 2) {
                throw CxsBadInput("Non canonical Cxs representation");
            } else {
                check_size(data, 3);
                uint16_t out;
                std::memcpy(&out, data.data() + 1, 2);
                data = data.subspan(3);
                if (out <= std::numeric_limits<uint8_t>::max()) {
                    throw CxsBadInput("Non canonical Cxs representation");
                }
                return out;
            }
        }
        case 2: {
            if constexpr (sizeof(T) < 4) {
                throw CxsBadInput("Non canonical Cxs representation");
            } else {
                check_size(data, 5);
                uint32_t out;
                std::memcpy(&out, data.data() + 1, 4);
                data = data.subspan(5);
                if (out <= std::numeric_limits<uint16_t>::max()) {
                    throw CxsBadInput("Non canonical Cxs representation");
                }
                return out;
            }
        }
        case 3: {
            if constexpr (sizeof(T) < 8) {
                throw CxsBadInput("Non canonical Cxs representation");
            } else {
                check_size(data, 9);
                uint64_t out;
                std::memcpy(&out, data.data() + 1 , 8);
                data = data.subspan(9);
                if (out <= std::numeric_limits<uint32_t>::max()) {
                    throw CxsBadInput("Non canonical Cxs representation");
                }
                return out;
            }
        }
        default:
            std::unreachable();
    }
}

inline void store_cxs_uint8(x23::Buf& buf, uint8_t value) {
    if (value <= 62) {
        buf.push_back(value);
        return;
    }
    buf.push_back(kCxsHeader1Byte); // 63
    buf.push_back(value);
}

inline void store_cxs_int8(x23::Buf& buf, int8_t value) {
    if (value < kCxsInt81ByteMin || value > kCxsInt81ByteMax) {
        // use 2 byte representation
        buf.push_back(kCxsHeader1Byte); // 63
        buf.push_back(value);
        return;
    }
    // use 1 byte representation
    if (value >= 0) {
        buf.push_back(value);
    } else {
        // invert and set highbit
        buf.push_back(((~value) & 0b0001'1111) | 0b0010'0000);
    }
}

inline void store_cxs_uint16(x23::Buf& buf, uint16_t value) {
    if (value > std::numeric_limits<uint8_t>::max()) {
        buf.push_back(kCxsHeader2Byte); // 127
        buf.resize_uninitialized(buf.size() + 2);
        std::memcpy(buf.data() + buf.size() - 2, &value, 2);
        return;
    }
    store_cxs_uint8(buf, value);
}

inline void store_cxs_int16(x23::Buf& buf, int16_t value) {
    if (value < std::numeric_limits<int8_t>::min() || value > std::numeric_limits<int8_t>::max()) {
        buf.push_back(kCxsHeader2Byte); // 127
        buf.resize_uninitialized(buf.size() + 2);
        std::memcpy(buf.data() + buf.size() - 2, &value, 2);
        return;
    }
    store_cxs_int8(buf, value);
}

inline void store_cxs_uint32(x23::Buf& buf, uint32_t value) {
    if (value > std::numeric_limits<uint16_t>::max()) {
        buf.push_back(kCxsHeader4Byte); // 191
        buf.resize_uninitialized(buf.size() + 4);
        std::memcpy(buf.data() + buf.size() - 4, &value, 4);
        return;
    }
    store_cxs_uint16(buf, value);
}

inline void store_cxs_int32(x23::Buf& buf, int32_t value) {
    if (value < std::numeric_limits<int16_t>::min() || value > std::numeric_limits<int16_t>::max()) {
        buf.push_back(kCxsHeader4Byte); // 191
        buf.resize_uninitialized(buf.size() + 4);
        std::memcpy(buf.data() + buf.size() - 4, &value, 4);
        return;
    }
    store_cxs_int16(buf, value);
}

inline void store_cxs_uint64(x23::Buf& buf, uint64_t value) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        buf.push_back(kCxsHeader8Byte); // 255
        buf.resize_uninitialized(buf.size() + 8);
        std::memcpy(buf.data() + buf.size() - 8, &value, 8);
        return;
    }
    store_cxs_uint32(buf, value);
}

inline void store_cxs_int64(x23::Buf& buf, int64_t value) {
    if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
        buf.push_back(kCxsHeader8Byte); // 255
        buf.resize_uninitialized(buf.size() + 8);
        std::memcpy(buf.data() + buf.size() - 8, &value, 8);
        return;
    }
    store_cxs_int32(buf, value);
}

inline void store_cxs_bool(x23::Buf& buf, bool value) {
    store_cxs_uint8(buf, value ? 1 : 0);
}

inline bool load_cxs_bool(std::span<const uint8_t>& data) {
    auto value = load_cxs_unsigned_integral<uint8_t>(data);
    if (value != 0 && value != 1) {
        throw CxsBadInput("Non canonical Cxs representation");
    }
    return value != 0;
}

template<std::signed_integral T>
T load_cxs_signed_integral(std::span<const uint8_t>& data) {
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
    switch(header) {
        case 0:
        {
            check_size(data, 2);
            int8_t out = data[1];
            data = data.subspan(2);
            if (out >= kCxsInt81ByteMin && out <= kCxsInt81ByteMax) {
                throw CxsBadInput("Non canonical Cxs representation");
            }
            return out;
        }
        case 1: {
            if constexpr (sizeof(T) < 2) {
                throw CxsBadInput("Non canonical Cxs representation");
            } else {
                check_size(data, 3);
                int16_t out;
                std::memcpy(&out, data.data() + 1, 2);
                data = data.subspan(3);
                if (out >= std::numeric_limits<int8_t>::min() && out <= std::numeric_limits<int8_t>::max()) {
                    throw CxsBadInput("Non canonical Cxs representation");
                }
                return out;
            }
        }
        case 2: {
            if constexpr (sizeof(T) < 4) {
                throw CxsBadInput("Non canonical Cxs representation");
            } else {
                check_size(data, 5);
                int32_t out;
                std::memcpy(&out, data.data() + 1, 4);
                data = data.subspan(5);
                if (out >= std::numeric_limits<int16_t>::min() && out <= std::numeric_limits<int16_t>::max()) {
                    throw CxsBadInput("Non canonical Cxs representation");
                }
                return out;
            }
        }
        case 3: {
            if constexpr (sizeof(T) < 8) {
                throw CxsBadInput("Non canonical Cxs representation");
            } else {
                check_size(data, 9);
                int64_t out;
                std::memcpy(&out, data.data() + 1, 8);
                data = data.subspan(9);
                if (out >= std::numeric_limits<int32_t>::min() && out <= std::numeric_limits<int32_t>::max()) {
                    throw CxsBadInput("Non canonical Cxs representation");
                }
                return out;
            }
        }
        default:
            std::unreachable();
    }
}

template<typename T>
struct CxsSerializer;

template<std::integral T>
struct CxsSerializer<T> {
    static void store(x23::Buf& buf, T value) {
        if constexpr (std::is_same_v<T, bool>) {
            store_cxs_bool(buf, value);
        } else if constexpr (std::is_same_v<T, int8_t>) {
            store_cxs_int8(buf, value);
        } else if constexpr (std::is_same_v<T, uint8_t>) {
            store_cxs_uint8(buf, value);
        } else if constexpr (std::is_same_v<T, int16_t>) {
            store_cxs_int16(buf, value);
        } else if constexpr (std::is_same_v<T, uint16_t>) {
            store_cxs_uint16(buf, value);
        } else if constexpr (std::is_same_v<T, int32_t>) {
            store_cxs_int32(buf, value);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            store_cxs_uint32(buf, value);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            store_cxs_int64(buf, value);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            store_cxs_uint64(buf, value);
        } else {
            static_assert(false, "impossible");
        }
    }
    static void load(std::span<const uint8_t>& data, T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            value = load_cxs_bool(data);
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            value = load_cxs_signed_integral<T>(data);
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            value = load_cxs_unsigned_integral<T>(data);
        } else {
            static_assert(false, "impossible");
        }
    }
};

template<typename T> requires (std::is_enum_v<T>)
struct CxsSerializer<T> {
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
        CxsSerializer<uint32_t>::store(buf, value.size());
        for (const auto& element : value) {
            CxsSerializer<typename X::value_type>::store(buf, element);
        }
    }
    static void load(std::span<const uint8_t>& data, X& value) {
        uint32_t size;
        CxsSerializer<uint32_t>::load(data, size);
        value.resize(size);
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

}