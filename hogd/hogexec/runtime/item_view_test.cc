#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "storage_object.hh"

namespace hogd {

TEST(ItemView, Exists)
{
  ItemView proxy(nullptr);
  EXPECT_FALSE(proxy.get_object_type());

  Bytestring bytes{0, 1, 2, 3};
  auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_FALSE(get_res);

  BytestringItem write1;
  write1.bytestring_ = bytes;

  EXPECT_TRUE(proxy.set(write1));
  ASSERT_TRUE(proxy.get_object_type());
  EXPECT_EQ(*proxy.get_object_type(), StorageObjectType::kBytestring);
}

TEST(ItemView, SetBytestring)
{
  ItemView proxy(nullptr);

  Bytestring bytes{0, 1, 2, 3}, bytes2{3, 2, 1, 0},
      bytes_long(kBytestringMaxLen + 10, 0);
  EXPECT_TRUE(proxy.set(BytestringItem(bytes)));

  auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->bytestring_, bytes);

  EXPECT_TRUE(proxy.set(BytestringItem(bytes2)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);

  EXPECT_FALSE(proxy.set(BytestringItem(bytes_long)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);
}

TEST(ItemView, NNIntSetAddPositiveSet)
{
  ItemView proxy(nullptr);

  ASSERT_FALSE(proxy.template get<NNIntItem>());
  EXPECT_TRUE(proxy.set(NNIntItem(100, 10)));

  auto *get_res = proxy.template get<NNIntItem>();

  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->base_value_, 100);
  EXPECT_EQ(get_res->add_value_, 10);

  EXPECT_FALSE(proxy.template get<BytestringItem>());
}

TEST(ItemView, NNIntSetAddValidation)
{
  ItemView proxy(nullptr);
  ASSERT_FALSE(proxy.template get<NNIntItem>());

  EXPECT_TRUE(proxy.set(NNIntItem(100, -10)));
  EXPECT_FALSE(proxy.set(NNIntItem(100, -110)));
  EXPECT_TRUE(proxy.set(NNIntItem(100, -100)));

  EXPECT_TRUE(proxy.set(NNIntItem(-10, 1)));
  EXPECT_TRUE(proxy.set(NNIntItem(-10, 0)));
  EXPECT_FALSE(proxy.set(NNIntItem(-10, -1)));

  EXPECT_TRUE(proxy.set(NNIntItem(0, 0)));
  EXPECT_FALSE(proxy.set(NNIntItem(0, -1)));
}

} // namespace hogd
