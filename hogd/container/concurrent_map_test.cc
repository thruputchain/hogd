#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <set>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/init.h"

namespace hogd {

struct TruncateHasher {
  uint64_t operator()(std::span<const uint8_t> key) const
  {
    uint64_t hash = 0;
    std::memcpy(&hash, key.data(), std::min(key.size(), sizeof(hash)));
    return std::byteswap(hash);
  }
};

TEST(ConcurrentMap, ValueBucketUsage)
{
  using bucket_t = detail::ValueBucket<std::vector<uint8_t>, int>;

  std::atomic<bucket_t *> value_bucket_ptr = nullptr;

  bucket_t::insert(&value_bucket_ptr, std::vector<uint8_t>{1, 2, 3}, 4);

  bucket_t::insert(&value_bucket_ptr, std::vector<uint8_t>{1, 2}, 5);
  bucket_t::insert(&value_bucket_ptr, std::vector<uint8_t>{1}, 6);

  // double inserts don't change the data structure
  bucket_t::insert(&value_bucket_ptr, std::vector<uint8_t>{1, 2}, 7);
  bucket_t::insert(&value_bucket_ptr, std::vector<uint8_t>{1, 2, 3}, 8);

  ASSERT_NE(value_bucket_ptr.load(), nullptr);
  ASSERT_EQ(value_bucket_ptr.load()->value_, 4);
  ASSERT_NE(value_bucket_ptr.load()->next_ptr_.load(), nullptr);
  ASSERT_EQ(value_bucket_ptr.load()->next_ptr_.load()->value_, 5);
  ASSERT_NE(value_bucket_ptr.load()->next_ptr_.load()->next_ptr_.load(),
            nullptr);
  ASSERT_EQ(value_bucket_ptr.load()->next_ptr_.load()->next_ptr_.load()->value_,
            6);
  ASSERT_EQ(value_bucket_ptr.load()
                ->next_ptr_.load()
                ->next_ptr_.load()
                ->next_ptr_.load(),
            nullptr);
}

static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 0) == 0x1);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 4) == 0x2);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 8) == 0x3);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 12) == 0x4);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 16) == 0x5);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 20) == 0x6);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 24) == 0x7);
static_assert(hogd::detail::Node<int, int, uint32_t>::get_branch_bits(
                  0x1234'5678, 28) == 0x8);

static_assert(hogd::detail::Node<int, int, uint16_t>::get_branch_bits(0x1234,
                                                                      0) ==
              0x1);
static_assert(hogd::detail::Node<int, int, uint16_t>::get_branch_bits(0x1234,
                                                                      4) ==
              0x2);
static_assert(hogd::detail::Node<int, int, uint16_t>::get_branch_bits(0x1234,
                                                                      8) ==
              0x3);
static_assert(hogd::detail::Node<int, int, uint16_t>::get_branch_bits(0x1234,
                                                                      12) ==
              0x4);

TEST(ConcurrentMap, InsertOrGet)
{
  EXPECT_EQ(TruncateHasher{}(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}),
            0x0000'0000'0000'0001);

  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;
  int *value =
      map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 4);
  ASSERT_EQ(*value, 4);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 5);
  ASSERT_EQ(*value, 4);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 2}, 6);
  ASSERT_EQ(*value, 6);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 3}, 7);
  ASSERT_EQ(*value, 7);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 4}, 8);
  ASSERT_EQ(*value, 8);
  value = map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 1, 0}, 9);
  ASSERT_EQ(*value, 9);
}

TEST(ConcurrentMap, ForEachParallel)
{
  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1}, 4);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 2}, 5);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 3}, 6);
  // separate bucket
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 4}, 7);
  map.insert_or_get(std::vector<uint8_t>{0, 0, 0, 1, 0, 0, 0, 5}, 8);

  std::mutex mtx;
  std::set<int> values;
  map.for_each_parallel([&values, &mtx](int &value) {
    std::scoped_lock lock(mtx);
    values.insert(value);
  });

  ASSERT_EQ(values.size(), 5);
  ASSERT_TRUE(values.contains(4));
  ASSERT_TRUE(values.contains(5));
  ASSERT_TRUE(values.contains(6));
  ASSERT_TRUE(values.contains(7));
  ASSERT_TRUE(values.contains(8));
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
