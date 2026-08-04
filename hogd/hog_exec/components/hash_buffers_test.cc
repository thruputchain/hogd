#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/components/hash_buffers.h"
#include "hog_exec_constants.hh"
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

} // namespace hogd
