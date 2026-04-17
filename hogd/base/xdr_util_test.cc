#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "x23/serialize.h"

namespace hogd {

TEST(Int128XdrBytesTest, MatchesXdrSerializeExactly)
{
  constexpr int128_t kValue =
      (int128_t{0x0102030405060708LL} << 64) | 0x1112131415161718ULL;

  auto encoded = int128_to_xdr_bytes(kValue);
  auto serialized = x23::serialize(to_xdr(kValue));
  std::span<const std::byte> serialized_bytes =
      std::as_bytes(std::span(serialized));

  ASSERT_EQ(encoded.size(), serialized_bytes.size());
  EXPECT_TRUE(
      std::equal(encoded.begin(), encoded.end(), serialized_bytes.begin()));
}

TEST(Int128XdrBytesTest, RoundTripsPositiveAndNegativeValues)
{
  constexpr int128_t kPositive =
      (int128_t{0x1122334455667788LL} << 64) | 0x99AABBCCDDEEFF00ULL;
  constexpr int128_t kNegative =
      -((int128_t{0x0123456789ABCDEFLL} << 64) | 0x0011223344556677ULL);

  EXPECT_EQ(int128_from_xdr_bytes(int128_to_xdr_bytes(kPositive)), kPositive);
  EXPECT_EQ(int128_from_xdr_bytes(int128_to_xdr_bytes(kNegative)), kNegative);
}

} // namespace hogd
