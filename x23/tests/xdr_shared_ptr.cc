#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "tests/xdrtest.hh"
#include "x23/xdr_shared_ptr.h"

using namespace x23;

namespace {

test_recursive
make_shared_ptr_test_value(std::uint64_t seed, size_t bytes)
{
  test_recursive value{};
  value.elem.resize(bytes);
  for (size_t i = 0; i < bytes; i++) {
    value.elem[i] = static_cast<char>('a' + (i + seed) % 26);
  }
  return value;
}

} // namespace

// [XdrSharedPtr] exists to change who owns the bytes, not what the bytes are,
// so pin the encoding against [Ptr] rather than trusting the type system
// to have meant it.
TEST(XdrSharedPtrTest, SerializesByteForByteLikeXdrPtr)
{
  const test_recursive value =
      make_shared_ptr_test_value(/*seed=*/7, /*bytes=*/37);

  const Ptr<test_recursive> owned(value);
  const XdrSharedPtr<test_recursive> shared(
      std::make_shared<const test_recursive>(value));

  EXPECT_EQ(xdr_size(shared), xdr_size(owned));
  EXPECT_EQ(serialize(shared), serialize(owned));
}

TEST(XdrSharedPtrTest, NullSerializesLikeNullXdrPtr)
{
  const Ptr<test_recursive> owned(nullptr);
  const XdrSharedPtr<test_recursive> shared;

  EXPECT_EQ(xdr_size(shared), 4UL);
  EXPECT_EQ(xdr_size(shared), xdr_size(owned));
  EXPECT_EQ(serialize(shared), serialize(owned));
}

TEST(XdrSharedPtrTest, VectorMatchesVectorOfXdrPtrs)
{
  const test_recursive first =
      make_shared_ptr_test_value(/*seed=*/1, /*bytes=*/0);
  const test_recursive second =
      make_shared_ptr_test_value(/*seed=*/2, /*bytes=*/64);

  // Present/absent/present, the shape a lookup reply actually produces.
  XdrVec<Ptr<test_recursive>> owned;
  owned.push_back(Ptr<test_recursive>(first));
  owned.push_back(nullptr);
  owned.push_back(Ptr<test_recursive>(second));

  XdrVec<XdrSharedPtr<test_recursive>> shared;
  shared.push_back(XdrSharedPtr<test_recursive>(
      std::make_shared<const test_recursive>(first)));
  shared.push_back({});
  shared.push_back(XdrSharedPtr<test_recursive>(
      std::make_shared<const test_recursive>(second)));

  EXPECT_EQ(xdr_size(shared), xdr_size(owned));
  EXPECT_EQ(serialize(shared), serialize(owned));
}

// A vector of these must be usable where the wire type is a vector of
// optionals -- that is what lets an RPC handler return one.
static_assert(serializes_like<XdrVec<XdrSharedPtr<test_recursive>>,
                              Vec<Ptr<test_recursive>, 0xffffffff>>);

TEST(XdrSharedPtrTest, SharesTheOriginalObjectRatherThanCopyingIt)
{
  const std::shared_ptr<const test_recursive> value =
      std::make_shared<const test_recursive>(
          make_shared_ptr_test_value(/*seed=*/3, /*bytes=*/16));

  const XdrSharedPtr<test_recursive> shared(value);

  EXPECT_EQ(shared.get(), value.get());
  EXPECT_EQ(value.use_count(), 2L);
}
