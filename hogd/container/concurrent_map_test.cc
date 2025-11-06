#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/container/merkle/allocator.h"
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
  BumpAllocator<bucket_t> allocator;

  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2, 3},
                   4);

  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2}, 5);
  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1}, 6);

  // double inserts don't change the data structure
  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2}, 7);
  bucket_t::insert(&value_bucket_ptr, allocator, std::vector<uint8_t>{1, 2, 3},
                   8);

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
  bucket_t::clear(value_bucket_ptr.load());
}

TEST(ConcurrentMap, DestructorLeakTest)
{
  ConcurrentMap<std::vector<uint8_t>, std::unique_ptr<int>, TruncateHasher> map;

  auto *value1 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                   std::make_unique<int>(4));
  ASSERT_EQ(**value1, 4);
  auto *value2 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(5));
  ASSERT_EQ(**value2, 5);
  auto *value3 =
      map.insert_or_get(std::vector<uint8_t>{1}, std::make_unique<int>(6));
  ASSERT_EQ(**value3, 6);
  auto *value4 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(7));
  ASSERT_EQ(**value4, 5);
  auto *value5 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                   std::make_unique<int>(8));
  ASSERT_EQ(**value5, 4);

  EXPECT_EQ(value1, value5);
  EXPECT_EQ(value2, value4);

  // make sure that duplicate inserts don't get leaked
  auto *value6 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                   std::make_unique<int>(4));
  ASSERT_EQ(**value6, 4);
  auto *value7 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(5));
  ASSERT_EQ(**value7, 5);
  auto *value8 =
      map.insert_or_get(std::vector<uint8_t>{1}, std::make_unique<int>(6));
  ASSERT_EQ(**value8, 6);
  auto *value9 =
      map.insert_or_get(std::vector<uint8_t>{1, 2}, std::make_unique<int>(7));
  ASSERT_EQ(**value9, 5);
  auto *value10 = map.insert_or_get(std::vector<uint8_t>{1, 2, 3},
                                    std::make_unique<int>(8));
  ASSERT_EQ(**value10, 4);

  EXPECT_EQ(value6, value10);
  EXPECT_EQ(value7, value9);

  EXPECT_EQ(value1, value6);
  EXPECT_EQ(value2, value7);
  EXPECT_EQ(value3, value8);
  EXPECT_EQ(value4, value9);
  EXPECT_EQ(value5, value10);
}

TEST(ConcurrentMap, ConcurrentInsert)
{
  // This is an imperfect test to check that if multiple threads attempt to
  // allocate bucket entries for the same entry, only one gets inserted, and the
  // rest get properly destroyed.
  ConcurrentMap<std::vector<uint8_t>, int, TruncateHasher> map;

  auto &pool = WorkerPool::get_default_pool();
  pool.for_each_parallel(
      std::ranges::iota_view(0, 128),
      [&map](int i) {
        uint8_t lastbit = i;
        std::vector<uint8_t> key = {0, 0, 0, 0, 0, 0, 0, lastbit};
        map.insert_or_get(key, i);
      },
      128);
}

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
  map.for_each_parallel(
      [&values, &mtx](std::pair<const std::vector<uint8_t> &, int &> value) {
        std::scoped_lock lock(mtx);
        values.insert(value.second);
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
