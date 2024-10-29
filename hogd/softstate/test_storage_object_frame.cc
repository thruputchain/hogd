#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hogd/softstate/storage_object_frame.h"
#include "storage_object.hh"

namespace hogd {

TEST(StorageObjectFrame, RewriteVariantTypeNullStart)
{
  StorageObjectFrame obj(std::nullopt);

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

  EXPECT_EQ(obj.try_reserve_delta(bytestring_delta),
            StorageReserveResult::kSuccess);

  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add_1),
            StorageReserveResult::kSuccess);

  obj.revert_delta(bytestring_delta);

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_1),
            StorageReserveResult::kSuccess);

  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add_2),
            StorageReserveResult::kSuccess);

  obj.revert_delta(nnint64_set_add_1);

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_2),
            StorageReserveResult::kSuccess);

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_3),
            StorageReserveResult::kSuccess);
  EXPECT_NE(obj.try_reserve_delta(nnint64_set_add_3),
            StorageReserveResult::kSuccess);

  obj.revert_delta(nnint64_set_add_3);

  EXPECT_EQ(obj.try_reserve_delta(nnint64_set_add_3),
            StorageReserveResult::kSuccess);
  obj.revert_delta(nnint64_set_add_3);

  obj.revert_delta(nnint64_set_add_2);

  EXPECT_EQ(obj.try_reserve_delta(bytestring_delta),
            StorageReserveResult::kSuccess);
}

} // namespace hogd
