#include "hogd/hog_indexer/event_cursor.h"

#include <algorithm>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "event_cursor_internal.hh"
#include "pagination.hh"
#include "types.hh"

namespace hogd {

namespace {

EventCursorPayload
make_payload(IterationOrder order, BlockHeight block, std::uint8_t tx_fill,
             std::uint32_t seq)
{
  EventCursorPayload p;
  p.schema_version = cursor::kCursorVersion;
  p.order = order;
  p.block_height = block;
  std::ranges::fill(p.tx_hash, tx_fill);
  p.sequence_number = seq;
  return p;
}

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(EventCursorTest, RoundTripAscending)
{
  auto in =
      make_payload(IterationOrder::IterationOrderAscending, 100, 0xAB, 42);
  std::string encoded = cursor::encode(in);
  auto decoded = cursor::decode(encoded);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(*decoded, in);
}

TEST(EventCursorTest, RoundTripDescending)
{
  auto in =
      make_payload(IterationOrder::IterationOrderDescending, 9999, 0x77, 0);
  std::string encoded = cursor::encode(in);
  auto decoded = cursor::decode(encoded);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(*decoded, in);
}
// NOLINTEND(bugprone-unchecked-optional-access)

TEST(EventCursorTest, EmptyInputRejected)
{
  EXPECT_FALSE(cursor::decode("").has_value());
}

TEST(EventCursorTest, OversizedInputRejected)
{
  std::string oversized(cursor::kMaxCursorBytes + 1, '\0');
  EXPECT_FALSE(cursor::decode(oversized).has_value());
}

TEST(EventCursorTest, GarbageInputRejected)
{
  // Random bytes that don't form a valid XDR EventCursorPayload.
  std::string garbage = "not a valid cursor at all";
  EXPECT_FALSE(cursor::decode(garbage).has_value());
}

TEST(EventCursorTest, VersionMismatchRejected)
{
  // Encode a payload, then patch its first 4 bytes (the
  // big-endian schema_version) to a value other than
  // kCursorVersion.  Decode should reject.
  auto in = make_payload(IterationOrder::IterationOrderAscending, 1, 0x01, 0);
  std::string encoded = cursor::encode(in);
  ASSERT_GE(encoded.size(), 4U);
  // XDR encodes uint32 big-endian at offset 0.  Set to 0xFFFFFFFF.
  encoded[0] = static_cast<char>(0xFF);
  encoded[1] = static_cast<char>(0xFF);
  encoded[2] = static_cast<char>(0xFF);
  encoded[3] = static_cast<char>(0xFF);
  EXPECT_FALSE(cursor::decode(encoded).has_value());
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(EventCursorTest, EncodeOverwritesSchemaVersion)
{
  // encode() always stamps `kCursorVersion`, even if the caller
  // passed a different value in the payload.
  auto in = make_payload(IterationOrder::IterationOrderAscending, 1, 0x01, 0);
  in.schema_version = 9999; // bogus, will be overwritten
  std::string encoded = cursor::encode(in);
  auto decoded = cursor::decode(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->schema_version, cursor::kCursorVersion);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
