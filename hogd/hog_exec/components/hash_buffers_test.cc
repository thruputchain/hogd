#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "syscall_error.hh"

namespace hogd {

namespace {

// The active-stream limit is configured by the chain.
constexpr uint32_t kMaxBuffers = 8;

} // namespace

TEST(HashBuffersTest, BasicUsage)
{
  HashBuffers buffers(kMaxBuffers);

  ASSERT_TRUE(buffers.update(0,
                             std::as_bytes(std::span<const uint8_t>(
                                 std::vector<uint8_t>{0, 1, 2, 3})),
                             true));
  ASSERT_TRUE(buffers.update(0,
                             std::as_bytes(std::span<const uint8_t>(
                                 std::vector<uint8_t>{4, 5, 6, 7})),
                             false));

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(HashBuffersTest, ResetUsage)
{
  HashBuffers buffers(kMaxBuffers);
  ASSERT_TRUE(buffers.update(0,
                             std::as_bytes(std::span<const uint8_t>(
                                 std::vector<uint8_t>{0, 1, 2, 3})),
                             true));
  ASSERT_TRUE(buffers.update(0,
                             std::as_bytes(std::span<const uint8_t>(
                                 std::vector<uint8_t>{4, 5, 6, 7})),
                             true));
  ASSERT_TRUE(buffers.update(0,
                             std::as_bytes(std::span<const uint8_t>(
                                 std::vector<uint8_t>{8, 9, 10, 11})),
                             false));

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(),
            sha256(std::vector<uint8_t>{4, 5, 6, 7, 8, 9, 10, 11}));
}

TEST(HashBuffersTest, MaxBuffersUsage)
{
  HashBuffers buffers(kMaxBuffers);
  for (uint8_t i = 0; i < kMaxBuffers; i++) {
    ASSERT_TRUE(
        buffers.update(i,
                       std::as_bytes(std::span<const uint8_t>(
                           std::vector<uint8_t>{static_cast<uint8_t>(i)})),
                       true));
  }
  ASSERT_FALSE(buffers.update(
      kMaxBuffers,
      std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})), true));
  ASSERT_TRUE(buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})),
      true));
}

TEST(HashBuffersTest, InvalidIndexUsage)
{
  HashBuffers buffers(kMaxBuffers);
  auto r = buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})),
      false);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

// The tests below pin behavior the contract can observe, independent of how
// the buffer is represented internally.

TEST(HashBuffersTest, BlockBoundaryChunkingMatchesOneShot)
{
  // Splitting the input at and around SHA-256's 64-byte block boundary must
  // not change the digest. These offsets are exactly where a streaming
  // implementation differs internally from an accumulate-then-hash one.
  std::vector<uint8_t> whole(200);
  for (size_t i = 0; i < whole.size(); i++) {
    whole[i] = static_cast<uint8_t>((i * 7) + 1);
  }
  const auto expected = sha256(whole);

  for (const size_t split :
       {1UL, 55UL, 56UL, 63UL, 64UL, 65UL, 127UL, 128UL, whole.size() - 1}) {
    HashBuffers buffers(kMaxBuffers);
    std::vector<uint8_t> head(whole.begin(), whole.begin() + split);
    std::vector<uint8_t> tail(whole.begin() + split, whole.end());
    ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(head)), true));
    ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(tail)), false));
    auto hash = buffers.finalize(0);
    ASSERT_TRUE(hash.has_value());
    EXPECT_EQ(hash.value(), expected) << "split at " << split;
  }
}

TEST(HashBuffersTest, FinalizeConsumesBuffer)
{
  HashBuffers buffers(kMaxBuffers);
  std::vector<uint8_t> data = {1, 2, 3};
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(data)), true));
  ASSERT_TRUE(buffers.finalize(0).has_value());

  // The buffer is gone: a second finalize and a no-reset update both fail.
  auto again = buffers.finalize(0);
  ASSERT_FALSE(again.has_value());
  ASSERT_EQ(again.error(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
  ASSERT_FALSE(buffers.update(0, std::as_bytes(std::span(data)), false));

  // The index is reusable with a fresh reset.
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(data)), true));
  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(data));
}

TEST(HashBuffersTest, FinalizeMissingIndexFails)
{
  HashBuffers buffers(kMaxBuffers);
  auto r = buffers.finalize(7);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST(HashBuffersTest, EmptyUpdatesDoNotChangeDigest)
{
  HashBuffers buffers(kMaxBuffers);
  std::vector<uint8_t> data = {9, 8, 7};
  ASSERT_TRUE(
      buffers.update(0, std::as_bytes(std::span<const uint8_t>()), true));
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(data)), false));
  ASSERT_TRUE(
      buffers.update(0, std::as_bytes(std::span<const uint8_t>()), false));

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(data));
}

TEST(HashBuffersTest, BeyondLegacySizeLimitMatchesOneShot)
{
  std::vector<uint8_t> whole(2 * kSHA256BufferMaxSize + 1);
  for (size_t i = 0; i < whole.size(); i++) {
    whole[i] = static_cast<uint8_t>((i * 31) + 5);
  }

  HashBuffers buffers(kMaxBuffers);
  const size_t half = whole.size() / 2;
  ASSERT_TRUE(buffers.update(
      0, std::as_bytes(std::span(whole.begin(), whole.begin() + half)), true));
  ASSERT_TRUE(buffers.update(
      0, std::as_bytes(std::span(whole.begin() + half, whole.end())), false));

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(whole));
}

TEST(HashBuffersTest, CopyThenDivergeMatchesOneShot)
{
  std::vector<uint8_t> prefix(100);
  for (size_t i = 0; i < prefix.size(); i++) {
    prefix[i] = static_cast<uint8_t>((i * 13) + 3);
  }
  const std::vector<uint8_t> suffix_a = {'a', 'a', 'a'};
  const std::vector<uint8_t> suffix_b = {'b', 'b'};

  std::vector<uint8_t> whole_a = prefix;
  whole_a.insert(whole_a.end(), suffix_a.begin(), suffix_a.end());
  std::vector<uint8_t> whole_b = prefix;
  whole_b.insert(whole_b.end(), suffix_b.begin(), suffix_b.end());

  HashBuffers buffers(kMaxBuffers);
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(prefix)), true));
  ASSERT_TRUE(buffers.copy(0, 1));
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(suffix_a)), false));
  ASSERT_TRUE(buffers.update(1, std::as_bytes(std::span(suffix_b)), false));

  auto hash_a = buffers.finalize(0);
  ASSERT_TRUE(hash_a.has_value());
  EXPECT_EQ(hash_a.value(), sha256(whole_a));

  auto hash_b = buffers.finalize(1);
  ASSERT_TRUE(hash_b.has_value());
  EXPECT_EQ(hash_b.value(), sha256(whole_b));
}

TEST(HashBuffersTest, CopyReplacesDestination)
{
  HashBuffers buffers(kMaxBuffers);
  const std::vector<uint8_t> src_data = {1, 2, 3};
  const std::vector<uint8_t> dst_data = {9, 9, 9, 9};
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(src_data)), true));
  ASSERT_TRUE(buffers.update(1, std::as_bytes(std::span(dst_data)), true));

  ASSERT_TRUE(buffers.copy(0, 1));

  auto hash = buffers.finalize(1);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(src_data));
}

TEST(HashBuffersTest, CopyMissingSourceFails)
{
  HashBuffers buffers(kMaxBuffers);
  const std::vector<uint8_t> dst_data = {7, 7};
  ASSERT_TRUE(buffers.update(1, std::as_bytes(std::span(dst_data)), true));

  auto r = buffers.copy(0, 1);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  // The failed copy left the destination untouched.
  auto hash = buffers.finalize(1);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(dst_data));
}

TEST(HashBuffersTest, CopyOntoSelfIsNoOp)
{
  HashBuffers buffers(kMaxBuffers);
  const std::vector<uint8_t> data = {4, 5, 6};
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(data)), true));

  ASSERT_TRUE(buffers.copy(0, 0));

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(data));
}

TEST(HashBuffersTest, CopyRespectsBufferCap)
{
  HashBuffers buffers(kMaxBuffers);
  const std::vector<uint8_t> data = {1};
  for (uint32_t i = 0; i < kMaxBuffers; i++) {
    ASSERT_TRUE(buffers.update(i, std::as_bytes(std::span(data)), true));
  }

  // A fresh destination would exceed the cap.
  auto r = buffers.copy(0, kMaxBuffers);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  // Copying onto an existing destination stays within the cap.
  ASSERT_TRUE(buffers.copy(0, kMaxBuffers - 1));

  // Finalizing one stream frees a slot for the copy.
  ASSERT_TRUE(buffers.finalize(1).has_value());
  ASSERT_TRUE(buffers.copy(0, kMaxBuffers));
}

TEST(HashBuffersTest, IndependentIndexesInterleaved)
{
  HashBuffers buffers(kMaxBuffers);
  std::vector<uint8_t> a1 = {1, 2}, a2 = {3, 4};
  std::vector<uint8_t> b1 = {5, 6}, b2 = {7, 8};

  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(a1)), true));
  ASSERT_TRUE(buffers.update(1, std::as_bytes(std::span(b1)), true));
  ASSERT_TRUE(buffers.update(0, std::as_bytes(std::span(a2)), false));
  ASSERT_TRUE(buffers.update(1, std::as_bytes(std::span(b2)), false));

  auto hash_b = buffers.finalize(1);
  ASSERT_TRUE(hash_b.has_value());
  EXPECT_EQ(hash_b.value(), sha256(std::vector<uint8_t>{5, 6, 7, 8}));

  auto hash_a = buffers.finalize(0);
  ASSERT_TRUE(hash_a.has_value());
  EXPECT_EQ(hash_a.value(), sha256(std::vector<uint8_t>{1, 2, 3, 4}));
}

// The cap comes from the constructed limit, not from kMaxBuffers: a chain
// configuring a different value gets that value enforced.
TEST(HashBuffersTest, ConfiguredBufferLimitIsEnforced)
{
  constexpr uint32_t kConfiguredMax = 2;
  HashBuffers buffers(kConfiguredMax);
  const std::vector<uint8_t> data{0x01};

  for (uint32_t i = 0; i < kConfiguredMax; i++) {
    ASSERT_TRUE(buffers.update(i, std::as_bytes(std::span(data)), true));
  }

  auto beyond =
      buffers.update(kConfiguredMax, std::as_bytes(std::span(data)), true);
  ASSERT_FALSE(beyond.has_value());
  EXPECT_EQ(beyond.error().code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  auto copied = buffers.copy(0, kConfiguredMax);
  ASSERT_FALSE(copied.has_value());
  EXPECT_EQ(copied.error().code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  // The same call a limit of 2 refused is admitted at a larger limit.
  HashBuffers larger(kConfiguredMax + 1);
  for (uint32_t i = 0; i < kConfiguredMax; i++) {
    ASSERT_TRUE(larger.update(i, std::as_bytes(std::span(data)), true));
  }
  EXPECT_TRUE(
      larger.update(kConfiguredMax, std::as_bytes(std::span(data)), true));
}

} // namespace hogd
