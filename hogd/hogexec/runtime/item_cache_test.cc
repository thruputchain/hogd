#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "storage_object.hh"

namespace hogd {

TEST(ItemCache, Exists)
{
  ItemCache proxy(std::nullopt);

  EXPECT_FALSE(proxy.exists());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  std::vector<uint8_t> bytes2{0, 1, 2, 3};

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  EXPECT_TRUE(proxy.exists());
  ASSERT_TRUE(!!proxy.get_object_type());

  EXPECT_EQ(proxy.get_object_type(), StorageObjectType::kBytestring);

  proxy.delete_last();

  EXPECT_TRUE(proxy.exists());

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes2))));
}

TEST(ItemCache, SetBytestring)
{
  ItemCache proxy(std::nullopt);

  auto eq = [](Bytestring const &bstring,
               std::vector<uint8_t> const &vec) -> bool {
    return (bstring.size() == vec.size() &&
            std::memcmp(bstring.data(), vec.data(), vec.size()) == 0);
  };

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  std::vector<uint8_t> bytes2{0, 1, 2, 3};

  std::vector<uint8_t> bytes_long(kBytestringMaxLen + 10, 0);

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  auto get_res = proxy.get_bytestring();

  ASSERT_TRUE(!!get_res);

  EXPECT_TRUE(eq(*get_res, bytes));

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes2))));

  ASSERT_TRUE(!!(get_res = proxy.get_bytestring()));
  EXPECT_TRUE(eq(*get_res, bytes2));

  EXPECT_FALSE(proxy.set_bytestring(std::as_bytes(std::span(bytes_long))));

  ASSERT_TRUE(!!(get_res = proxy.get_bytestring()));
  EXPECT_TRUE(eq(*get_res, bytes2));

  EXPECT_FALSE(!!proxy.nnint128_set_add(0, 1));
}

TEST(ItemCache, NNInt128SetAddPositiveSet)
{
  ItemCache proxy(std::nullopt);

  EXPECT_FALSE(!!proxy.get_nnint128_value());
  EXPECT_TRUE(!!proxy.nnint128_set_add(100, 10));

  auto res = proxy.get_nnint128_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 110);

  res = proxy.get_nnint128_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 100);

  EXPECT_FALSE(!!proxy.get_bytestring());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  EXPECT_FALSE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  EXPECT_TRUE(!!proxy.nnint128_set_add(100, -50));

  EXPECT_FALSE(!!proxy.nnint128_set_add(100, -80));
  EXPECT_FALSE(!!proxy.nnint128_set_add(110, 10));

  res = proxy.get_nnint128_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 100);

  res = proxy.get_nnint128_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 60);
}

TEST(ItemCache, NNInt128SetAddNegativeSet)
{
  ItemCache proxy(std::nullopt);

  EXPECT_FALSE(!!proxy.get_nnint128_value());
  EXPECT_TRUE(!!proxy.nnint128_set_add(-100, 10));

  auto res = proxy.get_nnint128_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -90);

  res = proxy.get_nnint128_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -100);

  EXPECT_FALSE(!!proxy.get_bytestring());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  EXPECT_FALSE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  EXPECT_FALSE(!!proxy.nnint128_set_add(-100, -50));

  EXPECT_TRUE(!!proxy.nnint128_set_add(-100, 10));

  res = proxy.get_nnint128_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -100);

  res = proxy.get_nnint128_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -80);
}

} // namespace hogd
