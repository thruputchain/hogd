#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hogd/softstate/storage_object_frame.h"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

TEST(StorageObjectFrame, RewriteVariantTypeNullStart)
{
  StorageObjectFrame obj(std::nullopt);

  // this test doesn't use the lock mechanism
  std::optional<StorageLock> lock = std::nullopt;

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  StorageDelta nnint64_set_add_1(StorageDeltaType::kNonnegativeInt64SetAdd);
  nnint64_set_add_1.nnint64().set_value = 0;
  nnint64_set_add_1.nnint64().add_value = 10;

  StorageDelta nnint64_set_add_2(StorageDeltaType::kNonnegativeInt64SetAdd);
  nnint64_set_add_2.nnint64().set_value = 5;
  nnint64_set_add_2.nnint64().add_value = 10;

  StorageDelta nnint64_set_add_3(StorageDeltaType::kNonnegativeInt64SetAdd);
  nnint64_set_add_3.nnint64().set_value = 5;
  nnint64_set_add_3.nnint64().add_value = -3;

  EXPECT_EQ(obj.try_reserve_delta(bytestring_delta, lock),
            StorageReserveResult::kSuccess);

  // type mismatch
  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add_1, lock),
            StorageReserveResult::kSuccess);

  obj.revert_delta(bytestring_delta);
  // object in neutral state

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_1, lock),
            StorageReserveResult::kSuccess);

  // mismatch in nnint64 set value
  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add_2, lock),
            StorageReserveResult::kSuccess);

  obj.revert_delta(nnint64_set_add_1);
  // object in neutral state

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_2, lock),
            StorageReserveResult::kSuccess);

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_3, lock),
            StorageReserveResult::kSuccess);
  // value would go below 0
  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add_3, lock),
            StorageReserveResult::kSuccess);

  obj.revert_delta(nnint64_set_add_3);

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_3, lock),
            StorageReserveResult::kSuccess);
  obj.revert_delta(nnint64_set_add_3);

  obj.revert_delta(nnint64_set_add_2);
  // object in neutral state

  EXPECT_EQ(obj.try_reserve_delta(bytestring_delta, lock),
            StorageReserveResult::kSuccess);
}

TEST(StorageObjectFrame, RewriteVariantFromBase)
{
  StorageObject base_object;

  // this test doesn't use the lock mechanism
  std::optional<StorageLock> lock = std::nullopt;

  base_object.type(StorageObjectType::kBytestring);
  base_object.bytestring() = {0, 1, 2, 3};

  StorageObjectFrame obj(base_object);

  StorageDelta bytestring_delta(StorageDeltaType::kBytestringSet);
  bytestring_delta.bytestring() = {3, 2, 1, 0};

  StorageDelta nnint64_set_add(StorageDeltaType::kNonnegativeInt64SetAdd);
  nnint64_set_add.nnint64().set_value = 0;
  nnint64_set_add.nnint64().add_value = 10;


  // updated delete semantics allows changing type:
  // checks for mismatched writes go in transaction runtime,
  // not the storage object frame
  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add, lock),
            StorageReserveResult::kSuccess);

  EXPECT_NE(obj.try_reserve_delta(bytestring_delta, lock),
            StorageReserveResult::kSuccess);
  obj.revert_delta(nnint64_set_add);

  EXPECT_EQ(obj.try_reserve_delta(bytestring_delta, lock),
            StorageReserveResult::kSuccess);

  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add, lock),
            StorageReserveResult::kSuccess);
  EXPECT_EQ(obj.try_reserve_delta(bytestring_delta, lock),
            StorageReserveResult::kSuccess);

  obj.commit_round();

  auto committed_obj = obj.get_committed_object();

  ASSERT_TRUE(committed_obj.has_value());

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(committed_obj->type(), StorageObjectType::kBytestring);

  EXPECT_EQ(committed_obj->bytestring(), bytestring_delta.bytestring());
  // NOLINTEND(bugprone-unchecked-optional-access)
}

} // namespace hogd
