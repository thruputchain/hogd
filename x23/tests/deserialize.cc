#include <gtest/gtest.h>

#include <span>
#include <tuple>
#include <utility>

#include <vector>
#include <x23/serialize.h>
#include <x23/serialize_cxs.h>
#include <x23/utility.h>
#include <x23/vec.h>

#include "tests/xdrtest.hh"
#include "x23/x23.h"

namespace x23 {

namespace {
template<typename... T>
x23::Buf
serialize_cxs_for_test(const T &...value)
{
  auto serialized = serialize_cxs(value...);
  if (!serialized) {
    ADD_FAILURE() << "CXS serialization failed: " << serialized.error();
    return {};
  }
  return std::move(*serialized);
}
} // namespace

// NOLINTBEGIN(modernize-type-traits)
TEST(Deserialize, Vector)
{
  x23::Vec<TxEventLog, 0xffffffff> vec;

  // Full buf in elts, from commented hex string
  // ceb73749c899693706ede1e30c9929b3fd5dd926163831c2fb8bd41e6efb112600000001746573745f6164647265737300000000000000000000000000000000000000000000000301020300
  x23::Buf buf = {
      0xce, 0xb7, 0x37, 0x49, 0xc8, 0x99, 0x69, 0x37, 0x06, 0xed, 0xe1,
      0xe3, 0x0c, 0x99, 0x29, 0xb3, 0xfd, 0x5d, 0xd9, 0x26, 0x16, 0x38,
      0x31, 0xc2, 0xfb, 0x8b, 0xd4, 0x1e, 0x6e, 0xfb, 0x11, 0x26, 0x00,
      0x00, 0x00, 0x01, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x61, 0x64, 0x64,
      0x72, 0x65, 0x73, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x00,
  };

  EXPECT_THROW(deserialize(&vec, BufView(buf)), x23::XdrBadInput);
}

// CXS BoundedStr deserialization must reject input where the decoded
// length exceeds the remaining span size (heap-buffer-overflow found
// by fuzz_deserialize).
TEST(Deserialize, CxsStrTooShort)
{
  uint8_t input[] = {0x0a}; // CXS-encodes length 10, but only 0 bytes follow
  std::span<const uint8_t> sp(input, sizeof(input));
  x23::Str val;
  auto status = x23::deserialize_cxs(&val, sp);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), CxsErrorCode::kTruncated);
}

// CXS reserved header bytes: any byte > 62 where the bottom 6 bits
// are not all 1s (i.e., not one of {63, 127, 191, 255}) is a reserved
// encoding per the CXS spec. These tests verify the deserializer rejects them.
TEST(Deserialize, CxsReservedUintHeader)
{
  // Byte 64 = 0b01000000: s=1, v=0 — reserved (v should be 63)
  // Followed by enough bytes that the code won't fail on size checks.
  uint8_t input[] = {64, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::span<const uint8_t> sp(input, sizeof(input));
  uint32_t val = 0;
  auto status = x23::deserialize_cxs(&val, sp);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), CxsErrorCode::kNonCanonical);
}

TEST(Deserialize, CxsReservedIntHeader)
{
  // Byte 100 = 0b01100100: s=1, v=36 — reserved (v should be 63)
  uint8_t input[] = {100, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::span<const uint8_t> sp(input, sizeof(input));
  int32_t val = 0;
  auto status = x23::deserialize_cxs(&val, sp);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), CxsErrorCode::kNonCanonical);
}

TEST(Deserialize, CxsReservedHeaderVariousBytes)
{
  // All of these have bottom 6 bits != 63 and value > 62, so they are
  // reserved.  Each should be rejected.
  for (uint8_t byte : {64, 65, 100, 126, 128, 190, 192, 200, 254}) {
    uint8_t input[] = {byte, 0, 0, 0, 0, 0, 0, 0, 0};
    std::span<const uint8_t> sp(input, sizeof(input));
    uint64_t uval = 0;
    EXPECT_FALSE(x23::deserialize_cxs(&uval, sp))
        << "byte " << static_cast<int>(byte) << " should be rejected for uint";
    int64_t ival = 0;
    EXPECT_FALSE(x23::deserialize_cxs(&ival, sp))
        << "byte " << static_cast<int>(byte) << " should be rejected for int";
  }
}

TEST(Deserialize, CxsBadDiscriminant)
{
  uint8_t input[] = {2};
  sunion_no_void value;
  auto status = x23::deserialize_cxs(&value, input);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), CxsErrorCode::kBadDiscriminant);
}

TEST(Deserialize, CxsTrailingData)
{
  uint8_t input[] = {1, 2};
  uint32_t value = 0;
  auto status = x23::deserialize_cxs(&value, input);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), CxsErrorCode::kTrailingData);

  auto used = x23::deserialize_cxs_from(&value, input);
  ASSERT_TRUE(used);
  EXPECT_EQ(*used, 1UZ);
  EXPECT_EQ(value, 1U);
}

TEST(Deserialize, OpaqueVectorTooLarge)
{
  x23::Vec<uint8_t, 5> vec;

  std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x09};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7, 8});
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrBadInput);

  x23::Vec<uint8_t, 4> vec2;

  std::vector<uint8_t> input2 = {0x00, 0x00, 0x00, 0x08};
  input2.insert(input2.end(), {0, 1, 2, 3, 4, 5, 6, 7});
  EXPECT_THROW(deserialize(&vec2, BufView(input2)), x23::XdrBadInput);
}

TEST(Deserialize, VectorTooLarge)
{
  x23::Vec<uint32_t, 1> vec;

  // valid encoding for a vector of length 2
  std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x02};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7});
  // LengthError not BadInput: see comment at serialize.h line 724
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrLengthError);
}

TEST(Deserialize, CxsVectorTooLarge)
{
  x23::Vec<uint32_t, 2> vec;

  // CXS uses little-endian size prefix
  std::vector<uint8_t> input = {0x03, 0x00, 0x00, 0x00};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7});
  std::span<const uint8_t> sp(input);
  auto status = x23::deserialize_cxs(&vec, sp);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), CxsErrorCode::kLengthExceeded);
}

// xdr_size(BorrowedPtr<T>) previously returned 0 for null, but xdr_serialize
// writes a 4-byte discriminant.  serialize_to uses xdr_size to allocate, so
// this triggers a heap-buffer-overflow under ASAN.
TEST(Serialize, NullBorrowedPtrSize)
{
  BorrowedPtr<uint32_t> bp;
  ASSERT_FALSE(bp);
  Buf buf = serialize(bp);
  // A null optional should serialize as a 4-byte zero discriminant.
  EXPECT_EQ(buf.size(), 4UZ);
}

namespace {
test_recursive
make_recursive_chain(size_t depth)
{
  test_recursive root;
  root.elem = "x";
  test_recursive *current = &root;
  for (size_t i = 1; i < depth; ++i) {
    current->next.emplace();
    current = &*current->next;
    current->elem = "x";
  }
  return root;
}
} // namespace

TEST(Deserialize, CxsRecursionLimit)
{
  size_t limit = deserialization_recursion_limit;

  // At exactly the limit, deserialization should succeed.
  {
    auto chain = make_recursive_chain(limit);
    auto buf = serialize_cxs_for_test(chain);
    test_recursive out;
    EXPECT_TRUE(deserialize_cxs(&out, BufView(buf)));
  }

  // One beyond the limit, deserialization should fail.
  {
    auto chain = make_recursive_chain(limit + 1);
    auto buf = serialize_cxs_for_test(chain);
    test_recursive out;
    auto status = deserialize_cxs(&out, BufView(buf));
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error(), CxsErrorCode::kRecursionLimit);
  }
}

TEST(Deserialize, XdrRecursionLimit)
{
  size_t limit = deserialization_recursion_limit;

  // At exactly the limit, deserialization should succeed.
  {
    auto chain = make_recursive_chain(limit);
    auto buf = serialize(chain);
    test_recursive out;
    EXPECT_NO_THROW(deserialize(&out, BufView(buf)));
  }

  // One beyond the limit, deserialization should throw.
  {
    auto chain = make_recursive_chain(limit + 1);
    auto buf = serialize(chain);
    test_recursive out;
    EXPECT_THROW(deserialize(&out, BufView(buf)), XdrBadInput);
  }
}

// Non-recursive types: CxsDeserializer<false> path (no recursion checker).
TEST(Deserialize, NonRecursiveRoundtrip)
{
  // Simple struct
  {
    fix_12 f{.i = 42, .d = 3.14};
    auto cxs_buf = serialize_cxs_for_test(f);
    fix_12 cxs_out;
    EXPECT_TRUE(deserialize_cxs(&cxs_out, BufView(cxs_buf)));
    EXPECT_EQ(f, cxs_out);

    auto xdr_buf = serialize(f);
    fix_12 xdr_out;
    EXPECT_NO_THROW(deserialize(&xdr_out, BufView(xdr_buf)));
    EXPECT_EQ(f, xdr_out);
  }
  // Union with data arm
  {
    StorageDelta sd(StorageDelta::kNonnegativeInt64SetAdd);
    sd.nnint64().set_value = 100;
    sd.nnint64().add_value = 200;
    auto cxs_buf = serialize_cxs_for_test(sd);
    StorageDelta cxs_out;
    EXPECT_TRUE(deserialize_cxs(&cxs_out, BufView(cxs_buf)));
    EXPECT_EQ(sd, cxs_out);

    auto xdr_buf = serialize(sd);
    StorageDelta xdr_out;
    EXPECT_NO_THROW(deserialize(&xdr_out, BufView(xdr_buf)));
    EXPECT_EQ(sd, xdr_out);
  }
  // Nested non-recursive struct
  {
    outer_struct os;
    os.mid.inner.x = 7;
    os.mid.inner.y = 2.5;
    os.mid.tag = "hello";
    os.count = 99;
    auto cxs_buf = serialize_cxs_for_test(os);
    outer_struct cxs_out;
    EXPECT_TRUE(deserialize_cxs(&cxs_out, BufView(cxs_buf)));
    EXPECT_EQ(os, cxs_out);

    auto xdr_buf = serialize(os);
    outer_struct xdr_out;
    EXPECT_NO_THROW(deserialize(&xdr_out, BufView(xdr_buf)));
    EXPECT_EQ(os, xdr_out);
  }
}

// Struct that contains a recursive type: recursion checking is enabled.
TEST(Deserialize, ContainsRecursiveLimit)
{
  size_t limit = deserialization_recursion_limit;

  // At the limit through the wrapper: wrapper is non-recursive so doesn't
  // consume a recursion slot, but the inner test_recursive chain does.
  {
    contains_test_recursive ct;
    ct.t = make_recursive_chain(limit);
    auto cxs_buf = serialize_cxs_for_test(ct);
    contains_test_recursive cxs_out;
    EXPECT_TRUE(deserialize_cxs(&cxs_out, BufView(cxs_buf)));

    auto xdr_buf = serialize(ct);
    contains_test_recursive xdr_out;
    EXPECT_NO_THROW(deserialize(&xdr_out, BufView(xdr_buf)));
  }
  // One beyond the limit.
  {
    contains_test_recursive ct;
    ct.t = make_recursive_chain(limit + 1);
    auto cxs_buf = serialize_cxs_for_test(ct);
    contains_test_recursive cxs_out;
    auto status = deserialize_cxs(&cxs_out, BufView(cxs_buf));
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error(), CxsErrorCode::kRecursionLimit);

    auto xdr_buf = serialize(ct);
    contains_test_recursive xdr_out;
    EXPECT_THROW(deserialize(&xdr_out, BufView(xdr_buf)), XdrBadInput);
  }
}

// Branching recursion via nextvec (tree, not just linked list).
// Each child in nextvec is a separate recursive entry.
TEST(Deserialize, BranchingRecursionLimit)
{
  size_t limit = deserialization_recursion_limit;

  // Root + (limit-1) children via nextvec = limit total entries.
  {
    test_recursive root;
    root.elem = "root";
    root.nextvec.resize(limit - 1);
    for (auto &child : root.nextvec)
      child.elem = "leaf";

    auto cxs_buf = serialize_cxs_for_test(root);
    test_recursive cxs_out;
    EXPECT_TRUE(deserialize_cxs(&cxs_out, BufView(cxs_buf)));

    auto xdr_buf = serialize(root);
    test_recursive xdr_out;
    EXPECT_NO_THROW(deserialize(&xdr_out, BufView(xdr_buf)));
  }
  // Root + limit children = limit+1 entries, exceeds limit.
  {
    test_recursive root;
    root.elem = "root";
    root.nextvec.resize(limit);
    for (auto &child : root.nextvec)
      child.elem = "leaf";

    auto cxs_buf = serialize_cxs_for_test(root);
    test_recursive cxs_out;
    auto status = deserialize_cxs(&cxs_out, BufView(cxs_buf));
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error(), CxsErrorCode::kRecursionLimit);

    auto xdr_buf = serialize(root);
    test_recursive xdr_out;
    EXPECT_THROW(deserialize(&xdr_out, BufView(xdr_buf)), XdrBadInput);
  }
}

// Mutually recursive types: MutualFrame -> MutualBody -> MutualFrame.
TEST(Deserialize, MutuallyRecursiveLimit)
{
  size_t limit = deserialization_recursion_limit;

  // Build a chain: Frame -> Body(success, child=Frame) -> Body(success,
  // child=Frame) -> ... Each MutualFrame and MutualBody are both recursive,
  // so each step consumes two recursion slots (one for the struct, one for the
  // union).
  auto make_mutual_chain = [](size_t depth) {
    MutualFrame root;
    root.body.which(0);
    root.body.success().x = 1;
    MutualFrame *cur = &root;
    for (size_t i = 1; i < depth; ++i) {
      cur->body.success().child.emplace();
      cur = &*cur->body.success().child;
      cur->body.which(0);
      cur->body.success().x = static_cast<int>(i);
    }
    return root;
  };

  // Each frame in the chain consumes 3 recursion slots: MutualFrame
  // (recursive struct) + MutualBody (recursive union) + the anonymous
  // success struct (also recursive). So limit/3 frames should fit.
  {
    auto chain = make_mutual_chain(limit / 3);
    auto cxs_buf = serialize_cxs_for_test(chain);
    MutualFrame cxs_out;
    EXPECT_TRUE(deserialize_cxs(&cxs_out, BufView(cxs_buf)));

    auto xdr_buf = serialize(chain);
    MutualFrame xdr_out;
    EXPECT_NO_THROW(deserialize(&xdr_out, BufView(xdr_buf)));
  }
  // One too many should fail.
  {
    auto chain = make_mutual_chain((limit / 3) + 1);
    auto cxs_buf = serialize_cxs_for_test(chain);
    MutualFrame cxs_out;
    auto status = deserialize_cxs(&cxs_out, BufView(cxs_buf));
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error(), CxsErrorCode::kRecursionLimit);

    auto xdr_buf = serialize(chain);
    MutualFrame xdr_out;
    EXPECT_THROW(deserialize(&xdr_out, BufView(xdr_buf)), XdrBadInput);
  }
}

// Opaque vec deserialization: roundtrip and trailing-zero checks for each
// padding length (rem = 0, 1, 2, 3).
TEST(Deserialize, OpaqueVecRoundtrip4Bytes)
{
  // 4 data bytes → rem=0, no padding, no check_trailing_zeros
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 4, 0xaa, 0xbb, 0xcc, 0xdd};
  EXPECT_NO_THROW(deserialize(&vec, BufView(input)));
  ASSERT_EQ(vec.size(), 4UZ);
  EXPECT_EQ(vec[0], 0xaa);
  EXPECT_EQ(vec[3], 0xdd);
}

TEST(Deserialize, OpaqueVecRoundtrip3Bytes)
{
  // 3 data bytes → rem=3, 1 padding byte must be zero
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 3, 0xaa, 0xbb, 0xcc, 0x00};
  EXPECT_NO_THROW(deserialize(&vec, BufView(input)));
  ASSERT_EQ(vec.size(), 3UZ);
  EXPECT_EQ(vec[0], 0xaa);
  EXPECT_EQ(vec[2], 0xcc);
}

TEST(Deserialize, OpaqueVecBadPad3Bytes)
{
  // 3 data bytes with non-zero padding → must reject
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 3, 0xaa, 0xbb, 0xcc, 0x01};
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrBadInput);
}

TEST(Deserialize, OpaqueVecRoundtrip2Bytes)
{
  // 2 data bytes → rem=2, 2 padding bytes must be zero
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 2, 0xaa, 0xbb, 0x00, 0x00};
  EXPECT_NO_THROW(deserialize(&vec, BufView(input)));
  ASSERT_EQ(vec.size(), 2UZ);
  EXPECT_EQ(vec[0], 0xaa);
  EXPECT_EQ(vec[1], 0xbb);
}

TEST(Deserialize, OpaqueVecBadPad2Bytes)
{
  // 2 data bytes with non-zero padding → must reject
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 2, 0xaa, 0xbb, 0x00, 0x01};
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrBadInput);
}

TEST(Deserialize, OpaqueVecRoundtrip1Byte)
{
  // 1 data byte → rem=1, 3 padding bytes must be zero
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 1, 0xaa, 0x00, 0x00, 0x00};
  EXPECT_NO_THROW(deserialize(&vec, BufView(input)));
  ASSERT_EQ(vec.size(), 1UZ);
  EXPECT_EQ(vec[0], 0xaa);
}

TEST(Deserialize, OpaqueVecBadPad1Byte)
{
  // 1 data byte with non-zero padding → must reject
  x23::Vec<uint8_t, 16> vec;
  std::vector<uint8_t> input = {0, 0, 0, 1, 0xaa, 0x01, 0x00, 0x00};
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrBadInput);
}

TEST(Deserialize, BufViewOpaqueRoundTrip)
{
  // Serialize a 5-byte opaque: 4-byte length (5) + 5 bytes + 3 pad = 12 bytes
  x23::Vec<uint8_t, 100> src{0x01, 0x02, 0x03, 0x04, 0x05};
  auto buf = x23::serialize(src);

  // Deserialize into a BufView (zero-copy)
  x23::BufView view;
  x23::deserialize(&view, x23::BufView(buf));

  EXPECT_EQ(view.size(), 5U);
  EXPECT_EQ(view[0], 0x01);
  EXPECT_EQ(view[4], 0x05);
}

TEST(Deserialize, BufViewEmptyOpaque)
{
  x23::Vec<uint8_t, 100> src{};
  auto buf = x23::serialize(src);

  x23::BufView view;
  x23::deserialize(&view, x23::BufView(buf));
  EXPECT_EQ(view.size(), 0U);
}

TEST(Deserialize, BufViewInTuple)
{
  uint32_t src_u32 = 42;
  x23::Vec<uint8_t, 100> src_opaque{0xAA, 0xBB, 0xCC};
  auto buf = x23::serialize(std::make_tuple(src_u32, src_opaque));

  std::tuple<uint32_t, x23::BufView> out;
  x23::deserialize(&out, x23::BufView(buf));
  EXPECT_EQ(std::get<0>(out), 42U);
  EXPECT_EQ(std::get<1>(out).size(), 3U);
  EXPECT_EQ(std::get<1>(out)[0], 0xAA);
}

TEST(Deserialize, BufViewPointsIntoInputBuffer)
{
  x23::Vec<uint8_t, 100> src{0x01, 0x02};
  auto buf = x23::serialize(src);

  x23::BufView view;
  x23::deserialize(&view, x23::BufView(buf));

  // BufView data pointer should be inside the buf memory region
  EXPECT_GE(view.data(), buf.data());
  EXPECT_LE(view.data() + view.size(), buf.data() + buf.size());
}

TEST(Deserialize, BufViewRejectsTruncatedInput)
{
  // Forge a 4-byte length header claiming 100 bytes, but only provide 8 bytes
  uint8_t bad_input[] = {0x00, 0x00, 0x00, 0x64,  // length = 100
                         0x01, 0x02, 0x03, 0x04}; // only 4 bytes of data
  x23::BufView view;
  EXPECT_THROW(x23::deserialize(&view,
                                x23::BufView(bad_input, sizeof(bad_input))),
               x23::XdrBadInput);
}

TEST(Deserialize, BufViewSerializesLikeCalldata)
{
  // Calldata = Vec<uint8_t, 0xffffffff>
  static_assert(
      x23::serializes_like<x23::BufView, x23::Vec<uint8_t, 0xffffffff>>);
  static_assert(x23::serializes_like<x23::BufView, x23::Vec<uint8_t, 100>>);
}

// A vector whose element is a zero-wire-size type (here an empty XDR struct)
// consumes no input per element, so a four-byte count must not be trusted to
// drive materialization: an unbounded count of 0xffffffff would otherwise grow
// the vector until std::bad_alloc, which escapes SRPC's noexcept input
// boundary.  The decoder caps materialization and rejects the oversized count
// as malformed XDR (which SRPC argument decode turns into GARBAGE_ARGS).
TEST(Deserialize, ZeroWireVectorCountRejectsBomb)
{
  static_assert(detail::xdr_size_bounds<std::tuple<>>().max_ == 0,
                "test relies on the empty struct being zero-wire-size");
  x23::Vec<std::tuple<>> vec;
  uint8_t input[] = {0xff, 0xff, 0xff, 0xff}; // count = 0xffffffff, no elements
  EXPECT_THROW(x23::deserialize(&vec, x23::BufView(input, sizeof(input))),
               x23::XdrBadInput);
}

// The cap must not reject legitimate encodings: a small count of zero-wire-size
// elements is a valid four-byte record that the serializer itself produces.
TEST(Deserialize, ZeroWireVectorCountAcceptsSmall)
{
  x23::Vec<std::tuple<>> vec;
  uint8_t input[] = {0x00, 0x00, 0x00, 0x03}; // count = 3, no trailing bytes
  x23::deserialize(&vec, x23::BufView(input, sizeof(input)));
  EXPECT_EQ(vec.size(), 3U);
}

// NOLINTEND(modernize-type-traits)

} // namespace x23
