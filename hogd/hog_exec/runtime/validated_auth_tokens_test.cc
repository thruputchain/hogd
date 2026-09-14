#include "hogd/hog_exec/runtime/validated_auth_tokens.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "syscall_error.hh"

namespace hogd {

namespace {

std::vector<std::byte>
to_bytes(std::span<uint32_t const> ids)
{
  std::vector<std::byte> out(ids.size_bytes());
  if (!ids.empty()) {
    std::memcpy(out.data(), ids.data(), ids.size_bytes());
  }
  return out;
}

} // namespace

TEST(ValidatedAuthTokensTest, ParseKeepsContractOrderAndBuildsSet)
{
  std::vector<uint32_t> const ids{7, 3, 9, 12};

  auto parsed = ValidatedAuthTokens::parse(to_bytes(ids));

  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(std::ranges::equal(parsed->ids(), ids));
  EXPECT_EQ(parsed->size(), ids.size());
  EXPECT_EQ(parsed->set().size(), ids.size());
  for (uint32_t id : ids) {
    EXPECT_TRUE(parsed->set().contains(id));
  }
}

TEST(ValidatedAuthTokensTest, ParseRejectsPartialId)
{
  auto bytes = to_bytes(std::vector<uint32_t>{1, 2});
  bytes.pop_back();

  auto parsed = ValidatedAuthTokens::parse(bytes);

  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST(ValidatedAuthTokensTest, ParseRejectsRepeatedId)
{
  auto bytes = to_bytes(std::vector<uint32_t>{1, 2, 3, 2});

  auto parsed = ValidatedAuthTokens::parse(bytes);

  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST(ValidatedAuthTokensTest, ParseAcceptsUnalignedSpan)
{
  std::vector<uint32_t> const ids{0x01020304, 0x0a0b0c0d};
  auto aligned = to_bytes(ids);
  std::vector<std::byte> shifted(aligned.size() + 1);
  std::memcpy(shifted.data() + 1, aligned.data(), aligned.size());

  auto parsed = ValidatedAuthTokens::parse(std::span(shifted).subspan(1));

  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(std::ranges::equal(parsed->ids(), ids));
}

TEST(ValidatedAuthTokensTest, EmptySpanParsesToEmptyList)
{
  auto parsed = ValidatedAuthTokens::parse(std::span<const std::byte>{});

  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->empty());
  EXPECT_TRUE(parsed->set().empty());
}

TEST(ValidatedAuthTokensTest, ReleaseHandsOutBothForms)
{
  std::vector<uint32_t> const ids{4, 8, 15};
  auto parsed = ValidatedAuthTokens::parse(to_bytes(ids));
  ASSERT_TRUE(parsed.has_value());

  auto [released_ids, released_set] = std::move(*parsed).release();

  EXPECT_EQ(released_ids, ids);
  EXPECT_EQ(released_set.size(), ids.size());
  for (uint32_t id : ids) {
    EXPECT_TRUE(released_set.contains(id));
  }
}

} // namespace hogd
