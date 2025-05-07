#include <cassert>
#include <cstring>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

class ItemViewTest : public ::testing::Test {
protected:
  static constexpr uint32_t kPortNumber = 12345;
  void SetUp() override
  {
    server_.start_server(kPortNumber);
    db_proxy_ = std::make_unique<DatabaseProxy>(cp_, LocalMockDB{kPortNumber});
  }

  BlockSnapshotServer server_ = BlockSnapshotServer(1);

  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::unique_ptr<DatabaseProxy> db_proxy_;

  std::unique_ptr<SnapshotStorageItem> make_empty_item()
  {
    assert(db_proxy_);
    return std::make_unique<SnapshotStorageItem>(
        StorageItem(StorageItemType::kItemTypeDeleted), AddressAndKey{},
        BlockID{1}, *db_proxy_);
  }
};

TEST_F(ItemViewTest, Exists)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());
  EXPECT_FALSE(proxy.get_item_type());

  Bytestring bytes{0, 1, 2, 3};
  const auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_FALSE(get_res);

  BytestringItem write1;
  write1.bytestring_ = bytes;

  EXPECT_TRUE(proxy.set(write1));
  ASSERT_TRUE(proxy.get_item_type());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(*proxy.get_item_type(), StorageItemType::kItemTypeBytestring);
}

TEST_F(ItemViewTest, SetBytestring)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());

  Bytestring bytes{0, 1, 2, 3}, bytes2{3, 2, 1, 0},
      bytes_long(kBytestringMaxLen + 10, 0);
  EXPECT_TRUE(proxy.set(BytestringItem(bytes)));

  const auto *get_res = proxy.template get<BytestringItem>();
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->bytestring_, bytes);

  EXPECT_TRUE(proxy.set(BytestringItem(bytes2)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);

  EXPECT_FALSE(proxy.set(BytestringItem(bytes_long)));
  ASSERT_TRUE(get_res = proxy.template get<BytestringItem>());
  EXPECT_EQ(get_res->bytestring_, bytes2);
}

TEST_F(ItemViewTest, NNIntSetAddPositiveSet)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());

  ASSERT_FALSE(proxy.template get<NNIntItem>());
  EXPECT_TRUE(proxy.set(NNIntItem(100, 10)));

  const auto *get_res = proxy.template get<NNIntItem>();

  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->base_value_, 100);
  EXPECT_EQ(get_res->add_value_, 10);

  EXPECT_FALSE(proxy.template get<BytestringItem>());
}

TEST_F(ItemViewTest, NNIntSetAddValidation)
{
  auto item = make_empty_item();
  ItemView proxy(item.get());
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
