#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

#include "hogd/base/types.h"
#include "hogd/hog_db/byte_arena.h"
#include "hogd/hog_db/materialized_change.h"

namespace hogd {

namespace {

std::span<const std::byte>
bytes(std::string_view s)
{
  return std::as_bytes(std::span(s));
}

// Build a kPut MaterializedChange the same way ChangeSet::put() does.
MaterializedChange
make_put(ByteArena &arena, std::string_view key, uint8_t type,
         std::string_view value)
{
  MaterializedChange change;
  change.op = MaterializedChange::Op::kPut;
  change.type = type;
  change.key_len = bytes(key).size();
  change.item = arena.store_item(bytes(key), bytes(value));
  return change;
}

} // namespace

// The arena stores only [key + value]; the ramdb type lives in the `type`
// field and must not consume a byte of the arena payload.
TEST(MaterializedChangeTest, PutLayoutHasNoTypeByte)
{
  ByteArena arena;
  auto change = make_put(arena, "somekey", 7, "somevalue");

  EXPECT_EQ(change.item.size(),
            bytes("somekey").size() + bytes("somevalue").size());
  EXPECT_TRUE(std::ranges::equal(change.key(), bytes("somekey")));
  EXPECT_TRUE(std::ranges::equal(change.value(), bytes("somevalue")));
  EXPECT_EQ(change.type, 7);
}

// value() must depend only on key_len, never on the type — the same
// key/value round-trips identically regardless of the type carried alongside.
TEST(MaterializedChangeTest, ValueIndependentOfType)
{
  ByteArena arena;
  auto a = make_put(arena, "k", 0, "payload");
  auto b = make_put(arena, "k", 255, "payload");

  EXPECT_TRUE(std::ranges::equal(a.value(), bytes("payload")));
  EXPECT_TRUE(std::ranges::equal(b.value(), bytes("payload")));
  EXPECT_TRUE(std::ranges::equal(a.value(), b.value()));
}

TEST(MaterializedChangeTest, EmptyKeyAndValue)
{
  ByteArena arena;

  auto empty_val = make_put(arena, "key", 3, "");
  EXPECT_EQ(empty_val.item.size(), bytes("key").size());
  EXPECT_TRUE(std::ranges::equal(empty_val.key(), bytes("key")));
  EXPECT_TRUE(empty_val.value().empty());

  auto empty_key = make_put(arena, "", 3, "val");
  EXPECT_EQ(empty_key.item.size(), bytes("val").size());
  EXPECT_TRUE(empty_key.key().empty());
  EXPECT_TRUE(std::ranges::equal(empty_key.value(), bytes("val")));
}

} // namespace hogd
