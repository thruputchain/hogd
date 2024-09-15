#include <gtest/gtest.h>

#include <optional>

#include "hogd/storage_proxy/types.h"

#include "hogd/storage_proxy/storage_object_proxy.h"

namespace hogd {

TEST(StorageObjectProxy, DeleteLastEmptyBase)
{
  std::optional<StorageObject> base_object = std::nullopt;

  StorageObjectProxy layer1(base_object);

  EXPECT_EQ(layer1.get_key_status(), KeyStatus::kKeyNExist);

  {
    StorageObjectProxy layer2(&layer1);

    EXPECT_EQ(layer2.get_key_status(), KeyStatus::kKeyNExist);

    EXPECT_TRUE(!!layer2.delete_last());

    EXPECT_EQ(layer2.get_key_status(), KeyStatus::kKeyAlreadyDeleted);

    // don't commit
  }

  EXPECT_EQ(layer1.get_key_status(), KeyStatus::kKeyNExist);

  {
    StorageObjectProxy layer2(&layer1);

    EXPECT_EQ(layer2.get_key_status(), KeyStatus::kKeyNExist);

    EXPECT_TRUE(!!layer2.delete_last());

    EXPECT_EQ(layer2.get_key_status(), KeyStatus::kKeyAlreadyDeleted);

    // commit
    layer2.commit_self_to_parent();
  }

  EXPECT_EQ(layer1.get_key_status(), KeyStatus::kKeyAlreadyDeleted);
}

} // namespace hogd
