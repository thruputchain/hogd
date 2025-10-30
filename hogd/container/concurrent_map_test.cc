#include <gtest/gtest.h>

#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/concurrent_map.h"

namespace hogd {

TEST(ConcurrentMap, ValueBucketUsage)
{
  using bucket_t = detail::ValueBucket<int>;
  bucket_t value_bucket(std::vector<uint8_t>{1, 2, 3}, 4);

  bucket_t::insert(&value_bucket, std::vector<uint8_t>{1, 2}, 5);
  bucket_t::insert(&value_bucket, std::vector<uint8_t>{1}, 6);

  // double inserts don't change the data structure
  bucket_t::insert(&value_bucket, std::vector<uint8_t>{1, 2}, 7);
  bucket_t::insert(&value_bucket, std::vector<uint8_t>{1, 2, 3}, 8);

  EXPECT_EQ(value_bucket.value_, 4);
  EXPECT_NE(value_bucket.next_ptr_.load(), nullptr);
  EXPECT_EQ(value_bucket.next_ptr_.load()->value_, 5);
  EXPECT_NE(value_bucket.next_ptr_.load()->next_ptr_.load(), nullptr);
  EXPECT_EQ(value_bucket.next_ptr_.load()->next_ptr_.load()->value_, 6);
  EXPECT_EQ(value_bucket.next_ptr_.load()->next_ptr_.load()->next_ptr_.load(),
            nullptr);
}

} // namespace hogd
