#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>

#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "xdrtest.hh"

namespace x23 {

TEST(CxsSerialize, Int8RoundTrip) {
    for (int8_t i = std::numeric_limits<int8_t>::min(); i != std::numeric_limits<int8_t>::max(); ++i) {
        x23::Buf buf;
        CxsSerializer<int8_t>::store(buf, i);
        auto view = x23::BufView(buf);
        if (i < kCxsInt81ByteMin || i > kCxsInt81ByteMax) {
            EXPECT_EQ(buf.size(), 2U);
        } else {
            EXPECT_EQ(buf.size(), 1U);
        }

        auto j = load_cxs_signed_integral<int8_t>(view);
        EXPECT_EQ(i, j);
    }
}

TEST(CxsSerialize, UInt8RoundTrip) {
    for (uint8_t i = 0; i != std::numeric_limits<uint8_t>::max(); ++i) {
        x23::Buf buf;
        CxsSerializer<uint8_t>::store(buf, i);
        auto view = x23::BufView(buf);
        if (i > kCxsUInt81ByteMax) {
            EXPECT_EQ(buf.size(), 2U);
        } else {
            EXPECT_EQ(buf.size(), 1U);
        }

        uint8_t j;
        CxsSerializer<uint8_t>::load(view, j);
        EXPECT_EQ(i, j);
    }
}

namespace {
template<typename T>
void check_roundtrip(T const& input) {
    x23::Buf buf;
    serialize_cxs(buf, input);
    auto view = x23::BufView(buf);
    T output;
    deserialize_cxs(view, output);
    EXPECT_EQ(input, output);
    EXPECT_EQ(view.size(), 0U); // check all data consumed
}
}

TEST(CxsSerialize, LargerIntRoundTrip) {
    std::random_device rd;
    std::uniform_int_distribution<int64_t> dist(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
    for (size_t count = 0; count < 10000; ++count) {
        int64_t i = dist(rd);
        check_roundtrip(i);
    }
}

// check uint edge cases
TEST(CxsSerialize, Uint32RoundTrip) {
    check_roundtrip(uint32_t{0});
    check_roundtrip(uint32_t{255});
    check_roundtrip(uint32_t{256});
    check_roundtrip(uint32_t{65535});
    check_roundtrip(uint32_t{65536});
    check_roundtrip(std::numeric_limits<uint32_t>::max());
}

TEST(CxsSerialize, Uint64RoundTrip) {
    check_roundtrip(uint64_t{0});
    check_roundtrip(uint64_t{255});
    check_roundtrip(uint64_t{256});
    check_roundtrip(uint64_t{65535});
    check_roundtrip(uint64_t{65536});
    check_roundtrip(uint64_t{1ULL << 32} - 1);
    check_roundtrip(uint64_t{1ULL << 32});
    check_roundtrip(std::numeric_limits<uint64_t>::max());
}

TEST(CxsSerialize, Int64RoundTrip) {
    check_roundtrip(int64_t{0});
    check_roundtrip(int64_t{0} - 1);
    
    check_roundtrip(int64_t{std::numeric_limits<int8_t>::max()});
    check_roundtrip(int64_t{std::numeric_limits<int8_t>::min()});
    check_roundtrip(int64_t{std::numeric_limits<int8_t>::max()} + 1);
    check_roundtrip(int64_t{std::numeric_limits<int8_t>::min()} - 1);

    check_roundtrip(int64_t{std::numeric_limits<int16_t>::max()});
    check_roundtrip(int64_t{std::numeric_limits<int16_t>::min()});
    check_roundtrip(int64_t{std::numeric_limits<int16_t>::max()} + 1);
    check_roundtrip(int64_t{std::numeric_limits<int16_t>::min()} - 1);

    check_roundtrip(int64_t{std::numeric_limits<int32_t>::max()});
    check_roundtrip(int64_t{std::numeric_limits<int32_t>::min()});
    check_roundtrip(int64_t{std::numeric_limits<int32_t>::max()} + 1);
    check_roundtrip(int64_t{std::numeric_limits<int32_t>::min()} - 1);

    check_roundtrip(std::numeric_limits<int64_t>::min());
    check_roundtrip(std::numeric_limits<int64_t>::max());
}

TEST(CxsSerialize, StructRoundTrip) {
    using test_struct = NonnegativeInt64SetAddArgs;
    test_struct value;
    value.set_value = 123;
    value.add_value = 456;
    check_roundtrip(value);
}

TEST(CxsSerialize, UnionRoundTrip) {
    using test_union = sunion_no_void;
    test_union value(test_union::SE_POSITIVE);
    value.pos() = 123;
    check_roundtrip(value);
}

TEST(CxsSerialize, VecRoundTrip) {
    using test_vec = Bytestring;
    test_vec value;
    value.append_range("hello");
    check_roundtrip(value);
}

TEST(CxsSerialize, StorageDeltaRoundTrip) {
    StorageDelta value(StorageDelta::kBytestringSet);
    x23::Buf buf;
    buf.append_range("world");
    value.bstring() = buf;
    check_roundtrip(value);
}

TEST(CxsSerialize, RecursiveRoundTrip) {
    using test_struct = test_recursive;
    test_struct value;
    value.elem = "hello";
    value.next.emplace();
    value.next->elem = "world";
    value.next->next.emplace();
    value.next->next->elem = "!";
    value.next->next->next.emplace();
    value.next->next->next->elem = "?";
    value.next->next->next->next.emplace();
    value.next->next->next->next->elem = ".";
    check_roundtrip(value);
}

TEST(CxsSerialize, OpaqueVecRoundTrip) {
    using test_array = x23::Vec<uint8_t, 1024>;
    test_array value;
    value.resize_uninitialized(256);
    for (size_t i = 0; i < 256; ++i) {
        value[i] = i;
    }
    check_roundtrip(value);
}

TEST(CxsSerialize, FloatRoundTrip) {
    check_roundtrip(float{0.0});
    check_roundtrip(float{1.0});
    check_roundtrip(float{-1.0});
    check_roundtrip(std::numeric_limits<float>::max());
    check_roundtrip(std::numeric_limits<float>::min());
}

TEST(CxsSerialize, CanonicalCheck) {
    x23::Buf buf;
    buf.push_back(kCxsHeader1Byte);
    buf.push_back(10);
    x23::BufView view(buf);
    uint8_t value;

    EXPECT_ANY_THROW(CxsSerializer<uint8_t>::load(view, value));
}

} // namespace x23