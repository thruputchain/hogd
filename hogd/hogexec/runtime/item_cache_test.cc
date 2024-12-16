#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "storage_object.hh"

namespace hogd {

TEST(CachedItem, Exists)
{
  CachedItem proxy(std::nullopt);

  EXPECT_FALSE(proxy.exists());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  std::vector<uint8_t> bytes2{0, 1, 2, 3};

  EXPECT_TRUE(proxy.bytestring_set(std::as_bytes(std::span(bytes))));

  EXPECT_TRUE(proxy.exists());
  ASSERT_TRUE(proxy.get_object_type());

  EXPECT_EQ(proxy.get_object_type(), StorageObjectType::kBytestring);
}

TEST(CachedItem, SetBytestring)
{
  CachedItem proxy(std::nullopt);

  auto eq = [](Bytestring const &bstring,
               std::vector<uint8_t> const &vec) -> bool {
    return (bstring.size() == vec.size() &&
            std::memcmp(bstring.data(), vec.data(), vec.size()) == 0);
  };

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  std::vector<uint8_t> bytes2{0, 1, 2, 3};

  std::vector<uint8_t> bytes_long(kBytestringMaxLen + 10, 0);

  EXPECT_TRUE(proxy.bytestring_set(std::as_bytes(std::span(bytes))));

  auto get_res = proxy.bytestring_get();

  ASSERT_TRUE(get_res);

  EXPECT_TRUE(eq(*get_res, bytes));

  EXPECT_TRUE(proxy.bytestring_set(std::as_bytes(std::span(bytes2))));

  ASSERT_TRUE(get_res = proxy.bytestring_get());
  EXPECT_TRUE(eq(*get_res, bytes2));

  EXPECT_FALSE(proxy.bytestring_set(std::as_bytes(std::span(bytes_long))));

  ASSERT_TRUE(get_res = proxy.bytestring_get());
  EXPECT_TRUE(eq(*get_res, bytes2));

  EXPECT_FALSE(proxy.nnint_set_add(0, 1));
}

TEST(CachedItem, NNIntSetAddPositiveSet)
{
  CachedItem proxy(std::nullopt);

  EXPECT_FALSE(proxy.nnint_get_value());
  EXPECT_TRUE(proxy.nnint_set_add(100, 10));

  auto res = proxy.nnint_get_value();
  ASSERT_TRUE(res);
  EXPECT_EQ(*res, 110);

  ASSERT_TRUE(res = proxy.nnint_get_base_value());
  EXPECT_EQ(*res, 100);

  EXPECT_FALSE(proxy.bytestring_get());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  EXPECT_FALSE(proxy.bytestring_set(std::as_bytes(std::span(bytes))));

  EXPECT_TRUE(proxy.nnint_set_add(100, -50));

  EXPECT_FALSE(proxy.nnint_set_add(100, -80));
  EXPECT_FALSE(proxy.nnint_set_add(110, 10));

  ASSERT_TRUE(res = proxy.nnint_get_base_value());
  EXPECT_EQ(*res, 100);

  ASSERT_TRUE(res = proxy.nnint_get_value());
  EXPECT_EQ(*res, 60);
}

TEST(CachedItem, NNIntSetAddNegativeSet)
{
  CachedItem proxy(std::nullopt);

  EXPECT_FALSE(proxy.nnint_get_value());
  EXPECT_TRUE(proxy.nnint_set_add(-100, 10));

  auto res = proxy.nnint_get_value();
  ASSERT_TRUE(res);
  EXPECT_EQ(*res, -90);

  ASSERT_TRUE(res = proxy.nnint_get_base_value());
  EXPECT_EQ(*res, -100);

  EXPECT_FALSE(proxy.bytestring_get());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  EXPECT_FALSE(proxy.bytestring_set(std::as_bytes(std::span(bytes))));

  EXPECT_FALSE(proxy.nnint_set_add(-100, -50));

  EXPECT_TRUE(proxy.nnint_set_add(-100, 10));

  ASSERT_TRUE(res = proxy.nnint_get_base_value());
  EXPECT_EQ(*res, -100);

  ASSERT_TRUE(res = proxy.nnint_get_value());
  EXPECT_EQ(*res, -80);
}

} // namespace hogd
