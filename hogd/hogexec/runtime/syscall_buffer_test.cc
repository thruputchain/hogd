#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hogd/hogexec/runtime/syscall_buffer.h"
#include "types.hh"

namespace hogd {
namespace {

std::span<const std::byte>
bytes_of(const std::array<uint8_t, 4> &a)
{
  return std::as_bytes(std::span(a));
}

// dst larger than src: copies all of src, returns src length, leaves the
// trailing dst bytes untouched.
TEST(CopyToGuestBuffer, BufferLargerThanSource)
{
  std::array<uint8_t, 4> src{0x11, 0x22, 0x33, 0x44};
  std::array<std::byte, 8> dst;
  dst.fill(std::byte{0xEE});

  RuntimeMemoryLength written = copy_to_guest_buffer(dst, bytes_of(src));

  EXPECT_EQ(written, 4u);
  EXPECT_EQ(dst[0], std::byte{0x11});
  EXPECT_EQ(dst[3], std::byte{0x44});
  EXPECT_EQ(dst[4], std::byte{0xEE}); // untouched
  EXPECT_EQ(dst[7], std::byte{0xEE}); // untouched
}

// dst exactly fits src: returns src length, fills dst.
TEST(CopyToGuestBuffer, BufferExactlyFitsSource)
{
  std::array<uint8_t, 4> src{0x11, 0x22, 0x33, 0x44};
  std::array<std::byte, 4> dst;
  dst.fill(std::byte{0xEE});

  RuntimeMemoryLength written = copy_to_guest_buffer(dst, bytes_of(src));

  EXPECT_EQ(written, 4u);
  EXPECT_EQ(dst[0], std::byte{0x11});
  EXPECT_EQ(dst[3], std::byte{0x44});
}

// dst smaller than src (truncation): returns the FULL src length (greater than
// dst.size()), copies exactly dst.size() bytes, and a sentinel just past dst is
// unaffected.
TEST(CopyToGuestBuffer, ReportsFullLengthWhenTruncated)
{
  std::array<uint8_t, 4> src{0x11, 0x22, 0x33, 0x44};
  std::array<std::byte, 3> backing; // dst is the first 2 bytes
  backing.fill(std::byte{0xEE});
  std::span<std::byte> dst(backing.data(), 2);

  RuntimeMemoryLength written = copy_to_guest_buffer(dst, bytes_of(src));

  EXPECT_EQ(written, 4u);         // full source length, not the buffer size
  EXPECT_GT(written, dst.size()); // signals truncation to the contract
  EXPECT_EQ(backing[0], std::byte{0x11});
  EXPECT_EQ(backing[1], std::byte{0x22});
  EXPECT_EQ(backing[2], std::byte{0xEE}); // one past dst — untouched
}

// empty dst, non-empty src: returns full src length, writes nothing.
TEST(CopyToGuestBuffer, EmptyBufferReportsFullLength)
{
  std::array<uint8_t, 4> src{0x11, 0x22, 0x33, 0x44};
  std::span<std::byte> dst; // empty

  RuntimeMemoryLength written = copy_to_guest_buffer(dst, bytes_of(src));

  EXPECT_EQ(written, 4u);
}

// empty src: returns 0.
TEST(CopyToGuestBuffer, EmptySourceReportsZero)
{
  std::array<std::byte, 8> dst;
  dst.fill(std::byte{0xEE});
  std::span<const std::byte> src; // empty

  RuntimeMemoryLength written = copy_to_guest_buffer(dst, src);

  EXPECT_EQ(written, 0u);
  EXPECT_EQ(dst[0], std::byte{0xEE}); // untouched
}

} // namespace
} // namespace hogd
