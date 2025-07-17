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
        store_cxs_int8(buf, i);
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

} // namespace x23