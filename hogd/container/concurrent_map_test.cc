#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/init.h"

namespace hogd {

TEST(ConcurrentMap, ValueBucketUsage)
{
  using bucket_t = detail::ValueBucket<int>;

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

static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 0) == 0x1);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 4) == 0x2);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 8) == 0x3);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 12) == 0x4);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 16) == 0x5);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 20) == 0x6);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 24) == 0x7);
static_assert(hogd::detail::Node<int>::get_branch_bits(0x1234'5678, 28) == 0x8);

TEST(ConcurrentMap, InsertOrGet)
{
  ConcurrentMap<int> map;
  auto key = std::vector<uint8_t>{1, 2, 3};
  int *value = map.insert_or_get(0x0000'0001, std::span(key), 4);
  ASSERT_EQ(*value, 4);
  value = map.insert_or_get(0x0000'0001, std::span(key), 5);
  ASSERT_EQ(*value, 4);
  value = map.insert_or_get(0x0000'0002, std::span(key), 6);
  ASSERT_EQ(*value, 6);
  value = map.insert_or_get(0x0000'0003, std::span(key), 7);
  ASSERT_EQ(*value, 7);
  value = map.insert_or_get(0x0000'0004, std::span(key), 8);
  ASSERT_EQ(*value, 8);
  value = map.insert_or_get(0x0001'0000, std::span(key), 9);
  ASSERT_EQ(*value, 9);
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
