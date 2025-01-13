#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/softstate/storage_item_frame.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// Change the typeclass of a storage item during execution
TEST(StorageItemFrame, RewriteVariantTypeNullStart)
{
  StorageItemFrame item(AddressAndKey{}, std::nullopt);

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  StorageDelta nnint_set_add_1 = make_nnint_delta(0, 10);
  StorageDelta nnint_set_add_2 = make_nnint_delta(5, 10);
  StorageDelta nnint_set_add_3 = make_nnint_delta(5, -3);

  EXPECT_TRUE(item.try_reserve_delta(bytestring_delta));

  // type mismatch
  EXPECT_FALSE(item.try_reserve_delta(nnint_set_add_1));
  item.revert_delta(bytestring_delta);

  // item in neutral state
  EXPECT_TRUE(item.try_reserve_delta(nnint_set_add_1));
  // mismatch in nnint set value
  EXPECT_FALSE(item.try_reserve_delta(nnint_set_add_2));
  item.revert_delta(nnint_set_add_1);

  // item in neutral state
  EXPECT_TRUE(item.try_reserve_delta(nnint_set_add_2));
  EXPECT_TRUE(item.try_reserve_delta(nnint_set_add_3));

  // fails because too much subtraction
  EXPECT_FALSE(item.try_reserve_delta(nnint_set_add_3));
  item.revert_delta(nnint_set_add_3);

  EXPECT_TRUE(item.try_reserve_delta(nnint_set_add_3));
  item.revert_delta(nnint_set_add_3);
  item.revert_delta(nnint_set_add_2);
  // item in neutral state

  EXPECT_TRUE(item.try_reserve_delta(bytestring_delta));
}

TEST(StorageItemFrame, RewriteVariantFromBase)
{
  StorageItem base_item;

  base_item.type(StorageItemType::kBytestring);
  base_item.bytestring() = {0, 1, 2, 3};

  StorageItemFrame item(AddressAndKey{}, base_item);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

  StorageDelta nnint_set_add = make_nnint_delta(0, 10);

  // updated delete semantics allows changing type:
  // checks for mismatched writes go in transaction runtime,
  // not the storage item frame
  EXPECT_TRUE(item.try_reserve_delta(nnint_set_add));

  EXPECT_FALSE(item.try_reserve_delta(bytestring_delta));
  item.revert_delta(nnint_set_add);

  // In neutral state now

  EXPECT_TRUE(item.try_reserve_delta(bytestring_delta));

  EXPECT_FALSE(item.try_reserve_delta(nnint_set_add));
  EXPECT_TRUE(item.try_reserve_delta(bytestring_delta));

  StorageItemFrame item2(AddressAndKey{}, item.get_post_close_block_item());

  auto committed_item = item2.get_snapshot_item();

  ASSERT_TRUE(committed_item.has_value());

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(committed_item->type(), StorageItemType::kBytestring);

  EXPECT_EQ(committed_item->bytestring(), bytestring_delta.bytestring());
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(StorageItemFrame, DeleteItem)
{
  StorageItem base_item;

  base_item.type(StorageItemType::kBytestring);
  base_item.bytestring() = {0, 1, 2, 3};

  StorageItemFrame item(AddressAndKey{}, base_item);
  ASSERT_TRUE(item.get_snapshot_item());

  // committing an empty round keeps item active
  StorageItemFrame item2(AddressAndKey{}, item.get_post_close_block_item());

  ASSERT_TRUE(item2.get_snapshot_item());

  EXPECT_TRUE(item2.try_reserve_delta(StorageDelta(StorageDeltaType::kDelete)));

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};
  EXPECT_FALSE(item2.try_reserve_delta(bytestring_delta));

  item2.commit_delta(StorageDelta(StorageDeltaType::kDelete));

  StorageItemFrame item3(AddressAndKey{}, item2.get_post_close_block_item());

  EXPECT_FALSE(item3.get_snapshot_item());
}

} // namespace hogd
