#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hogd/container/flat_key_set.h"

namespace hogd {
namespace {

// Minimal key matching FrameItemKey's shape, with a settable hash so tests
// can force collisions.
struct TestKey {
  static constexpr size_t kBytes = 68;
  using Bytes = std::array<uint8_t, kBytes>;

  Bytes bytes_{};
  size_t hash_ = 0;

  size_t hash() const noexcept { return hash_; }
  Bytes const &bytes() const noexcept { return bytes_; }
  bool operator==(TestKey const &other) const noexcept
  {
    return hash_ == other.hash_ && bytes_ == other.bytes_;
  }
};

static_assert(!std::is_copy_constructible_v<FlatKeySet<TestKey>>);
static_assert(!std::is_copy_assignable_v<FlatKeySet<TestKey>>);
static_assert(std::is_nothrow_move_constructible_v<FlatKeySet<TestKey>>);
static_assert(std::is_nothrow_move_assignable_v<FlatKeySet<TestKey>>);

struct TestKeyHash {
  size_t operator()(TestKey const &key) const noexcept { return key.hash(); }
};

uint64_t
mix(uint64_t x)
{
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

enum class HashMode : uint8_t {
  // A well-distributed 64-bit hash, the production case.
  kDistinct,
  // Every key hashes to 0, which is also the table's empty-slot tag.
  kAllZero,
  // Four hash values for all keys: the byte compare has to do the work.
  kFourValues,
};

TestKey
key_of(uint64_t index, HashMode mode = HashMode::kDistinct)
{
  TestKey key;
  std::memcpy(key.bytes_.data(), &index, sizeof(index));
  const uint64_t tail = mix(index);
  std::memcpy(key.bytes_.data() + TestKey::kBytes - sizeof(tail), &tail,
              sizeof(tail));
  switch (mode) {
  case HashMode::kDistinct:
    key.hash_ = mix(index ^ 0xabcdefULL);
    break;
  case HashMode::kAllZero:
    key.hash_ = 0;
    break;
  case HashMode::kFourValues:
    key.hash_ = mix(index) & 3;
    break;
  }
  return key;
}

TEST(FlatKeySetTest, StartsEmpty)
{
  FlatKeySet<TestKey> set;
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set.size(), 0U);
  EXPECT_EQ(set.capacity(), 0U);
  EXPECT_FALSE(set.contains(key_of(0)));
}

// insert() answers the way std::unordered_set::insert(key).second does, which
// is what SystemCallHandler::note_item_load() turns into the load charge.
TEST(FlatKeySetTest, InsertReportsFirstInsertionOnly)
{
  FlatKeySet<TestKey> set;
  EXPECT_TRUE(set.insert(key_of(1)));
  EXPECT_FALSE(set.insert(key_of(1)));
  EXPECT_TRUE(set.insert(key_of(2)));
  EXPECT_FALSE(set.insert(key_of(2)));
  EXPECT_FALSE(set.insert(key_of(1)));
  EXPECT_EQ(set.size(), 2U);
  EXPECT_TRUE(set.contains(key_of(1)));
  EXPECT_TRUE(set.contains(key_of(2)));
  EXPECT_FALSE(set.contains(key_of(3)));
}

// A key rebuilt from the same parts is the same key: the syscall path builds
// a fresh key per call and expects to find the one inserted earlier.
TEST(FlatKeySetTest, FindsAnEqualKeyBuiltSeparately)
{
  FlatKeySet<TestKey> set;
  EXPECT_TRUE(set.insert(key_of(42)));
  const TestKey again = key_of(42);
  EXPECT_TRUE(set.contains(again));
  EXPECT_FALSE(set.insert(again));
}

// Same hash, different bytes: two keys, not one. The hash decides where to
// look, never whether two keys are equal.
TEST(FlatKeySetTest, DistinguishesKeysThatShareAHash)
{
  FlatKeySet<TestKey> set;
  TestKey a = key_of(1);
  TestKey b = key_of(2);
  b.hash_ = a.hash_;
  EXPECT_TRUE(set.insert(a));
  EXPECT_TRUE(set.insert(b));
  EXPECT_EQ(set.size(), 2U);
  EXPECT_TRUE(set.contains(a));
  EXPECT_TRUE(set.contains(b));
  EXPECT_FALSE(set.insert(a));
  EXPECT_FALSE(set.insert(b));
}

// Membership must match std::unordered_set across growth, under each hash
// mode.
class FlatKeySetDifferentialTest : public ::testing::TestWithParam<HashMode> {};

TEST_P(FlatKeySetDifferentialTest, MatchesUnorderedSetAcrossGrowth)
{
  const HashMode mode = GetParam();
  // The colliding modes probe O(n) per key, so they run shorter.
  const uint64_t rounds = mode == HashMode::kDistinct ? 100000 : 3000;

  FlatKeySet<TestKey> set;
  std::unordered_set<TestKey, TestKeyHash> reference;
  std::mt19937_64 rng(static_cast<uint64_t>(mode) + 1);
  std::vector<TestKey> inserted;
  for (uint64_t round = 0; round < rounds; ++round) {
    const bool repeat = !inserted.empty() && rng() % 4 == 0;
    const uint64_t index = repeat ? rng() % inserted.size() : round;
    const TestKey key = key_of(index, mode);
    const bool expected = reference.insert(key).second;
    ASSERT_EQ(set.insert(key), expected) << "round " << round;
    if (expected) {
      inserted.push_back(key);
    }
    ASSERT_EQ(set.size(), reference.size()) << "round " << round;
  }
  for (auto const &key : inserted) {
    EXPECT_TRUE(set.contains(key));
  }
  for (uint64_t index = rounds; index < rounds + 1000; ++index) {
    EXPECT_FALSE(set.contains(key_of(index, mode)));
  }
  // Load never exceeds one half.
  EXPECT_GE(set.capacity(), set.size() * 2);
}

INSTANTIATE_TEST_SUITE_P(HashModes, FlatKeySetDifferentialTest,
                         ::testing::Values(HashMode::kDistinct,
                                           HashMode::kAllZero,
                                           HashMode::kFourValues));

TEST(FlatKeySetTest, MoveTransfersContents)
{
  FlatKeySet<TestKey> set;
  for (uint64_t i = 0; i < 100; ++i) {
    set.insert(key_of(i));
  }
  FlatKeySet<TestKey> moved(std::move(set));
  EXPECT_EQ(moved.size(), 100U);
  for (uint64_t i = 0; i < 100; ++i) {
    EXPECT_TRUE(moved.contains(key_of(i)));
  }
}

} // namespace
} // namespace hogd
