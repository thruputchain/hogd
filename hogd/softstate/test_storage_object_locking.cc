#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>

#include "hogd/hogexec/runtime/item_cache.h"
#include "hogd/softstate/storage_object_frame.h"
#include "storage_object.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class StorageObjectFrameLockingTest : public ::testing::Test {
protected:
  void SetUp() override
  {

    bstring_1 = StorageDelta(StorageDeltaType::kBytestringSet);
    bstring_1.bytestring() = bytes;

    bstring_2 = StorageDelta(StorageDeltaType::kBytestringSet);
    bstring_2.bytestring() = bytes2;
  }

  // for setting bytestring vars
  x23::Vec<uint8_t> bytes = x23::Vec<uint8_t>{0, 1, 2, 3};
  x23::Vec<uint8_t> bytes2 = x23::Vec<uint8_t>{3, 2, 1, 0};

  StorageDelta bstring_1, bstring_2;
};

TEST_F(StorageObjectFrameLockingTest, SimpleLockCommit)
{
  StorageObjectFrame frame(std::nullopt);
  ItemCache proxy_1(std::nullopt);
  ItemCache proxy_2(std::nullopt);

  EXPECT_TRUE(proxy_1.set_bytestring(std::as_bytes(std::span(bytes))));
  EXPECT_TRUE(proxy_2.set_bytestring(std::as_bytes(std::span(bytes2))));

  auto change_lock_1 = proxy_1.compute_storage_lock_acquire();

  ASSERT_TRUE(change_lock_1);

  EXPECT_EQ(frame.try_change_lock(*change_lock_1),
            StorageReserveResult::kSuccess);

  proxy_1.mark_locks_acquired();

  std::optional<StorageLock> null_lock = std::nullopt;

  EXPECT_NE(frame.try_reserve_delta(bstring_2, null_lock),
            StorageReserveResult::kSuccess);

  auto proxy_lock = proxy_1.get_stripped_lock();
  auto deltas = proxy_1.get_deltas();

  ASSERT_EQ(deltas.size(), 1);

  EXPECT_EQ(frame.try_reserve_delta(deltas[0], proxy_lock),
            StorageReserveResult::kSuccess);

  // afterwards cannot also reserve a conflicting delta
  EXPECT_NE(frame.try_reserve_delta(bstring_2, null_lock),
            StorageReserveResult::kSuccess);

  // afterwards cannot acquire a conflicting lock

  auto change_lock_2 = proxy_2.compute_storage_lock_acquire();

  ASSERT_TRUE(change_lock_2);

  EXPECT_NE(frame.try_change_lock(*change_lock_2),
            StorageReserveResult::kSuccess);

  // but if the delta reverts, then lock can be acquired

  frame.revert_delta(bstring_2);

  EXPECT_EQ(frame.try_change_lock(*change_lock_2),
            StorageReserveResult::kSuccess);
}

TEST_F(StorageObjectFrameLockingTest, ExclusiveLockAcquireCommit)
{
  StorageObjectFrame frame(std::nullopt);
  ItemCache proxy_1(std::nullopt);

  EXPECT_TRUE(proxy_1.set_bytestring(std::as_bytes(std::span(bytes))));

  auto lock = proxy_1.compute_storage_lock_acquire();

  ASSERT_TRUE(lock);
  EXPECT_EQ(frame.try_change_lock(*lock), StorageReserveResult::kSuccess);

  proxy_1.mark_locks_acquired();

  EXPECT_TRUE(proxy_1.set_bytestring(std::as_bytes(std::span(bytes2))));

  lock = proxy_1.compute_storage_lock_acquire();
  ASSERT_TRUE(lock);
  EXPECT_EQ(frame.try_change_lock(*lock), StorageReserveResult::kSuccess);
  proxy_1.mark_locks_acquired();

  // even a write of the same (top-level) value can't succeed,
  // because proxy_1 has an exclusive lock
  ItemCache proxy_2(std::nullopt);

  EXPECT_TRUE(proxy_2.set_bytestring(std::as_bytes(std::span(bytes2))));

  lock = proxy_2.compute_storage_lock_acquire();

  ASSERT_TRUE(lock);
  EXPECT_NE(frame.try_change_lock(*lock), StorageReserveResult::kSuccess);

  auto deltas = proxy_1.get_deltas();
  ASSERT_EQ(deltas.size(), 1);
  auto stripped_lock = proxy_1.get_stripped_lock();

  EXPECT_EQ(frame.try_reserve_delta(deltas[0], stripped_lock),
            StorageReserveResult::kSuccess);

  // now lock is released, proxy_2 can reserve successfully

  EXPECT_EQ(frame.try_change_lock(*lock), StorageReserveResult::kSuccess);

  // but proxy_2 can't acquire an exclusive lock

  proxy_2.mark_locks_acquired();

  EXPECT_TRUE(proxy_2.set_bytestring(std::as_bytes(std::span(bytes))));

  lock = proxy_2.compute_storage_lock_acquire();

  ASSERT_TRUE(lock);

  EXPECT_NE(frame.try_change_lock(*lock), StorageReserveResult::kSuccess);
}

} // namespace hogd
