#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "syscall_error.hh"

namespace hogd {

TEST(HashBuffersTest, BasicUsage)
{
  HashBuffers buffers;

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
  HashBuffers buffers;
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
  HashBuffers buffers;
  for (uint8_t i = 0; i < kMaxSHA256Buffers; i++) {
    ASSERT_TRUE(
        buffers.update(i,
                       std::as_bytes(std::span<const uint8_t>(
                           std::vector<uint8_t>{static_cast<uint8_t>(i)})),
                       true));
  }
  ASSERT_FALSE(buffers.update(
      kMaxSHA256Buffers,
      std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})), true));
  ASSERT_TRUE(buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})),
      true));
}

TEST(HashBuffersTest, InvalidIndexUsage)
{
  HashBuffers buffers;
  auto r = buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})),
      false);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST(HashBuffersTest, InvalidBufferSizeUsage)
{
  HashBuffers buffers;

  ASSERT_TRUE(buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{})),
      true));
  for (unsigned int i = 0; i < kSHA256BufferMaxSize; i++) {
    uint8_t data = i & 0xFF;
    ASSERT_TRUE(buffers.update(
        0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{data})),
        false));
  }
  auto r = buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})),
      false);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST(HashBuffersTest, InvalidSize)
{
  HashBuffers buffers;
  std::vector<uint8_t> too_big_buffer =
      std::vector<uint8_t>(kSHA256BufferMaxSize + 1, 0);
  ASSERT_FALSE(
      buffers.update(0, std::as_bytes(std::span<const uint8_t>(too_big_buffer)),
                     false));
  std::vector<uint8_t> not_too_big_buffer =
      std::vector<uint8_t>(kSHA256BufferMaxSize, 0);
  ASSERT_TRUE(buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(std::vector<uint8_t>{0})),
      true));
  ASSERT_FALSE(buffers.update(
      0, std::as_bytes(std::span<const uint8_t>(not_too_big_buffer)), false));
}

TEST(HashBuffersTest, FailedResetPreservesExistingBuffer)
{
  HashBuffers buffers;

  std::vector<uint8_t> prefix = {0x41, 0x42, 0x43, 0x44};
  ASSERT_TRUE(
      buffers.update(0, std::as_bytes(std::span<const uint8_t>(prefix)), true));

  std::vector<uint8_t> oversized(kSHA256BufferMaxSize + 1, 0xFF);
  auto result =
      buffers.update(0, std::as_bytes(std::span<const uint8_t>(oversized)),
                     true);
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(result.error(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(prefix));
}

// The tests below pin behavior the contract can observe, independent of how
// the buffer is represented internally (accumulated bytes today, a streaming
// hash state tomorrow): digests, the size caps and their error paths, the
// finalize-consumes lifecycle, and the byte count finalize reports.

namespace {

std::span<const std::byte>
bytes_of(const std::vector<uint8_t> &v)
{
  return std::as_bytes(std::span<const uint8_t>(v));
}

} // namespace

TEST(HashBuffersTest, BlockBoundaryChunkingMatchesOneShot)
{
  // Splitting the input at and around SHA-256's 64-byte block boundary must
  // not change the digest. These offsets are exactly where a streaming
  // implementation differs internally from an accumulate-then-hash one.
  std::vector<uint8_t> whole(200);
  for (size_t i = 0; i < whole.size(); i++) {
    whole[i] = static_cast<uint8_t>(i * 7 + 1);
  }
  const auto expected = sha256(whole);

  for (const size_t split : {1UL, 55UL, 56UL, 63UL, 64UL, 65UL, 127UL, 128UL,
                             whole.size() - 1}) {
    HashBuffers buffers;
    std::vector<uint8_t> head(whole.begin(), whole.begin() + split);
    std::vector<uint8_t> tail(whole.begin() + split, whole.end());
    ASSERT_TRUE(buffers.update(0, bytes_of(head), true));
    ASSERT_TRUE(buffers.update(0, bytes_of(tail), false));
    auto hash = buffers.finalize(0);
    ASSERT_TRUE(hash.has_value());
    EXPECT_EQ(hash.value(), expected) << "split at " << split;
  }
}

TEST(HashBuffersTest, FailedAppendPreservesExistingBuffer)
{
  // The size check must reject an overflowing append before absorbing any of
  // its bytes: the buffer keeps hashing as if the failed call never happened.
  HashBuffers buffers;
  std::vector<uint8_t> prefix = {0x10, 0x20, 0x30};
  ASSERT_TRUE(buffers.update(0, bytes_of(prefix), true));

  std::vector<uint8_t> oversized(kSHA256BufferMaxSize, 0xEE);
  auto result = buffers.update(0, bytes_of(oversized), false);
  ASSERT_FALSE(result.has_value());
  ASSERT_EQ(result.error(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  std::vector<uint8_t> suffix = {0x40};
  ASSERT_TRUE(buffers.update(0, bytes_of(suffix), false));

  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(std::vector<uint8_t>{0x10, 0x20, 0x30, 0x40}));
}

TEST(HashBuffersTest, FinalizeConsumesBuffer)
{
  HashBuffers buffers;
  std::vector<uint8_t> data = {1, 2, 3};
  ASSERT_TRUE(buffers.update(0, bytes_of(data), true));
  ASSERT_TRUE(buffers.finalize(0).has_value());

  // The buffer is gone: a second finalize and a no-reset update both fail.
  auto again = buffers.finalize(0);
  ASSERT_FALSE(again.has_value());
  ASSERT_EQ(again.error(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
  ASSERT_FALSE(buffers.update(0, bytes_of(data), false));

  // The index is reusable with a fresh reset.
  ASSERT_TRUE(buffers.update(0, bytes_of(data), true));
  auto hash = buffers.finalize(0);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(data));
}

TEST(HashBuffersTest, FinalizeMissingIndexFails)
{
  HashBuffers buffers;
  auto r = buffers.finalize(7);
  ASSERT_FALSE(r.has_value());
  ASSERT_EQ(r.error(), SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST(HashBuffersTest, FinalizeReportsAccumulatedSize)
{
  // finalize's size output feeds sha256_total_bytes_processed in transaction
  // stats, so the byte count is observable and must survive representation
  // changes: it counts bytes since the last reset, not since creation.
  HashBuffers buffers;

  size_t size = 12345;
  ASSERT_TRUE(buffers.update(0, bytes_of({}), true));
  ASSERT_TRUE(buffers.finalize(0, &size).has_value());
  EXPECT_EQ(size, 0U);

  ASSERT_TRUE(buffers.update(0, bytes_of(std::vector<uint8_t>(10, 1)), true));
  ASSERT_TRUE(buffers.update(0, bytes_of(std::vector<uint8_t>(20, 2)), false));
  ASSERT_TRUE(buffers.update(0, bytes_of(std::vector<uint8_t>(30, 3)), false));
  ASSERT_TRUE(buffers.finalize(0, &size).has_value());
  EXPECT_EQ(size, 60U);

  ASSERT_TRUE(buffers.update(0, bytes_of(std::vector<uint8_t>(40, 4)), true));
  ASSERT_TRUE(buffers.update(0, bytes_of(std::vector<uint8_t>(5, 5)), true));
  ASSERT_TRUE(buffers.finalize(0, &size).has_value());
  EXPECT_EQ(size, 5U);
}

TEST(HashBuffersTest, EmptyUpdatesDoNotChangeDigest)
{
  HashBuffers buffers;
  std::vector<uint8_t> data = {9, 8, 7};
  ASSERT_TRUE(buffers.update(0, bytes_of({}), true));
  ASSERT_TRUE(buffers.update(0, bytes_of(data), false));
  ASSERT_TRUE(buffers.update(0, bytes_of({}), false));

  size_t size = 0;
  auto hash = buffers.finalize(0, &size);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(data));
  EXPECT_EQ(size, data.size());
}

TEST(HashBuffersTest, MaxSizeBufferDigestMatchesOneShot)
{
  // Exactly kSHA256BufferMaxSize bytes is accepted, hashes correctly, and
  // reports the full size.
  std::vector<uint8_t> whole(kSHA256BufferMaxSize);
  for (size_t i = 0; i < whole.size(); i++) {
    whole[i] = static_cast<uint8_t>(i * 31 + 5);
  }

  HashBuffers buffers;
  const size_t half = whole.size() / 2;
  ASSERT_TRUE(buffers.update(
      0, bytes_of({whole.begin(), whole.begin() + half}), true));
  ASSERT_TRUE(
      buffers.update(0, bytes_of({whole.begin() + half, whole.end()}), false));

  size_t size = 0;
  auto hash = buffers.finalize(0, &size);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(hash.value(), sha256(whole));
  EXPECT_EQ(size, kSHA256BufferMaxSize);
}

TEST(HashBuffersTest, IndependentIndexesInterleaved)
{
  HashBuffers buffers;
  std::vector<uint8_t> a1 = {1, 2}, a2 = {3, 4};
  std::vector<uint8_t> b1 = {5, 6}, b2 = {7, 8};

  ASSERT_TRUE(buffers.update(0, bytes_of(a1), true));
  ASSERT_TRUE(buffers.update(1, bytes_of(b1), true));
  ASSERT_TRUE(buffers.update(0, bytes_of(a2), false));
  ASSERT_TRUE(buffers.update(1, bytes_of(b2), false));

  auto hash_b = buffers.finalize(1);
  ASSERT_TRUE(hash_b.has_value());
  EXPECT_EQ(hash_b.value(), sha256(std::vector<uint8_t>{5, 6, 7, 8}));

  auto hash_a = buffers.finalize(0);
  ASSERT_TRUE(hash_a.has_value());
  EXPECT_EQ(hash_a.value(), sha256(std::vector<uint8_t>{1, 2, 3, 4}));
}

} // namespace hogd
