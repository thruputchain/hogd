#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "storage_object.hh"
#include "types.hh"

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

  proxy.delete_object();

  EXPECT_FALSE(proxy.exists());

  EXPECT_EQ(proxy.get_object_type(), StorageObjectType::kDeleted);

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes2))));

  EXPECT_EQ(proxy.get_object_type(), StorageObjectType::kBytestring);

  EXPECT_TRUE(proxy.exists());
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

  EXPECT_FALSE(!!proxy.nnint64_set_add(0, 1));
}

TEST(ItemCache, NNInt64SetAddPositiveSet)
{
  ItemCache proxy(std::nullopt);

  EXPECT_FALSE(!!proxy.get_nnint64_value());
  EXPECT_TRUE(!!proxy.nnint64_set_add(100, 10));

  auto res = proxy.get_nnint64_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 110);

  res = proxy.get_nnint64_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 100);

  EXPECT_FALSE(!!proxy.get_bytestring());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  EXPECT_FALSE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  EXPECT_TRUE(!!proxy.nnint64_set_add(100, -50));

  EXPECT_FALSE(!!proxy.nnint64_set_add(100, -80));
  EXPECT_FALSE(!!proxy.nnint64_set_add(110, 10));

  res = proxy.get_nnint64_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 100);

  res = proxy.get_nnint64_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, 60);
}

TEST(ItemCache, NNInt64SetAddNegativeSet)
{
  ItemCache proxy(std::nullopt);

  EXPECT_FALSE(!!proxy.get_nnint64_value());
  EXPECT_TRUE(!!proxy.nnint64_set_add(-100, 10));

  auto res = proxy.get_nnint64_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -90);

  res = proxy.get_nnint64_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -100);

  EXPECT_FALSE(!!proxy.get_bytestring());

  std::vector<uint8_t> bytes{0, 1, 2, 3};
  EXPECT_FALSE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  EXPECT_FALSE(!!proxy.nnint64_set_add(-100, -50));

  EXPECT_TRUE(!!proxy.nnint64_set_add(-100, 10));

  res = proxy.get_nnint64_base_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -100);

  res = proxy.get_nnint64_value();
  ASSERT_TRUE(!!res);
  EXPECT_EQ(*res, -80);
}

TEST(ItemCache, ReplayCacheFromNone)
{
  ItemCache proxy(std::nullopt);

  EXPECT_TRUE(proxy.replay_cache_insert(0));
  EXPECT_TRUE(proxy.replay_cache_insert(1));
  EXPECT_FALSE(proxy.replay_cache_insert(1));

  EXPECT_TRUE(proxy.replay_cache_clear(1));

  EXPECT_TRUE(proxy.replay_cache_clear(10));

  EXPECT_FALSE(proxy.replay_cache_insert(5));

  EXPECT_TRUE(proxy.replay_cache_insert(10));
}

TEST(ItemCache, ReplayCacheFromSome)
{
  StrippedStorageObject object;
  object.type(StorageObjectType::kReplayCache);
  object.replay_cache_metadata().clear_threshold = 10;

  ItemCache proxy(object);

  EXPECT_TRUE(proxy.replay_cache_insert(15));
  EXPECT_FALSE(proxy.replay_cache_insert(5));
  EXPECT_TRUE(proxy.replay_cache_insert(10));
}

namespace {
MessageQueueEvent
make_event(uint64_t value)
{
  return test::make_calldata(value);
}
} // namespace

TEST(ItemCache, MessageQueueAppendConsumeFromEmpty)
{
  ItemCache proxy(std::nullopt);

  EXPECT_TRUE(proxy.append_event_to_mq(make_event(0)));
  EXPECT_TRUE(proxy.append_event_to_mq(make_event(1)));
  EXPECT_TRUE(proxy.append_event_to_mq(make_event(2)));
  EXPECT_TRUE(proxy.append_event_to_mq(make_event(3)));

  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(0));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(1));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(2));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(3));

  EXPECT_FALSE(proxy.consume_next_mq_event());
}

TEST(ItemCache, MessageQueueConsumeFromNonempty)
{
  StrippedStorageObject object(StorageObjectType::kMessageQueue);

  object.message_queue().events.push_back(make_event(0));
  object.message_queue().events.push_back(make_event(1));
  object.message_queue().events.push_back(make_event(2));
  object.message_queue().events.push_back(make_event(3));

  ItemCache proxy(object);

  EXPECT_TRUE(proxy.append_event_to_mq(make_event(4)));

  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(0));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(1));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(2));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(3));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(4));

  EXPECT_FALSE(proxy.consume_next_mq_event());
}

TEST(ItemCache, MessageQueueConcurrentOutputsSorted)
{
  StrippedStorageObject object(StorageObjectType::kMessageQueue);

  object.message_queue().events.push_back(make_event(0));
  object.message_queue().events.push_back(make_event(15));
  object.message_queue().events.push_back(make_event(1));
  object.message_queue().events.push_back(make_event(14));

  ItemCache proxy(object);

  EXPECT_TRUE(proxy.append_event_to_mq(make_event(4)));
  EXPECT_TRUE(proxy.append_event_to_mq(make_event(7)));
  EXPECT_TRUE(proxy.append_event_to_mq(make_event(5)));

  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(0));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(15));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(1));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(14));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(4));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(5));
  EXPECT_EQ(*proxy.consume_next_mq_event(), make_event(7));

  EXPECT_FALSE(proxy.consume_next_mq_event());
}

TEST(ItemCache, NoLockSubInt64Min)
{
  ItemCache proxy(std::nullopt);

  EXPECT_TRUE(!!proxy.nnint64_set_add(INT64_MAX, -INT64_MAX));
  EXPECT_FALSE(!!proxy.nnint64_set_add(INT64_MAX, -1));
}

TEST(ItemCache, ExclusiveTypeClassLock)
{
  ItemCache proxy(std::nullopt);

  EXPECT_FALSE(proxy.compute_storage_lock_acquire().has_value());

  EXPECT_TRUE(!!proxy.nnint64_set_add(100, 10));

  auto lock = proxy.compute_storage_lock_acquire();
  ASSERT_TRUE(lock.has_value());
  EXPECT_EQ(lock->previous_type, StorageLockType::kNone);
  EXPECT_EQ(lock->new_type, StorageLockType::kSetObjectTypeClass);
  ASSERT_TRUE(!!lock->new_typeclass);
  // no type-specific lock data yet required
  EXPECT_EQ(lock->marginal_extra_lock.nnint64_subtracted_amount(), 0);

  EXPECT_TRUE(!!proxy.nnint64_set_add(100, -20));

  lock = proxy.compute_storage_lock_acquire();
  ASSERT_TRUE(lock.has_value());
  EXPECT_EQ(lock->previous_type, StorageLockType::kNone);
  EXPECT_EQ(lock->new_type, StorageLockType::kSetObjectTypeClass);
  ASSERT_TRUE(!!lock->new_typeclass);
  ASSERT_EQ(lock->new_typeclass->type(), StorageObjectType::kNonnegativeInt64);
  EXPECT_EQ(lock->new_typeclass->int64(), 100);
  // must reserve subtraction of -10
  EXPECT_EQ(lock->marginal_extra_lock.nnint64_subtracted_amount(), -10);

  {
    // child frame should inherit locks, not implicitly assume parent acquired
    // locks
    ItemCache proxy_child_nolocks_acquired(proxy);

    proxy_child_nolocks_acquired.delete_object();

    auto lock = proxy_child_nolocks_acquired.compute_storage_lock_acquire();

    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(lock->previous_type, StorageLockType::kNone);
    EXPECT_EQ(lock->new_type, StorageLockType::kSetObjectTypeClass);

    EXPECT_EQ(lock->new_typeclass->type(), StorageObjectType::kDeleted);
  }

  proxy.mark_locks_acquired();

  {
    // child frame should inherit locks, not implicitly assume parent acquired
    // locks
    ItemCache proxy_child_locks_acquired(proxy);

    proxy_child_locks_acquired.delete_object();

    auto lock = proxy_child_locks_acquired.compute_storage_lock_acquire();

    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(lock->previous_type, StorageLockType::kSetObjectTypeClass);
    EXPECT_EQ(lock->new_type, StorageLockType::kExclusiveLock);

    EXPECT_FALSE(!!lock->new_typeclass);
  }

  // now check unwind negation

  EXPECT_FALSE(!!proxy.compute_storage_lock_acquire());

  lock = proxy.compute_storage_lock_release();

  ASSERT_TRUE(lock.has_value());
  EXPECT_EQ(lock->previous_type, StorageLockType::kSetObjectTypeClass);
  EXPECT_EQ(lock->new_type, StorageLockType::kNone);
  EXPECT_FALSE(!!lock->new_typeclass);
  // must release subtraction of -10
  EXPECT_EQ(lock->marginal_extra_lock.nnint64_subtracted_amount(), 10);

  // adding doesn't require extra locks, but subtracting does

  EXPECT_TRUE(!!proxy.nnint64_set_add(100, 5));
  EXPECT_FALSE(!!proxy.compute_storage_lock_acquire());

  EXPECT_TRUE(!!proxy.nnint64_set_add(100, -10));
  lock = proxy.compute_storage_lock_acquire();

  ASSERT_TRUE(lock.has_value());
  EXPECT_EQ(lock->previous_type, StorageLockType::kSetObjectTypeClass);
  EXPECT_EQ(lock->new_type, StorageLockType::kSetObjectTypeClass);
  EXPECT_FALSE(!!lock->new_typeclass);
  // must reserve subtraction of -5
  EXPECT_EQ(lock->marginal_extra_lock.nnint64_subtracted_amount(), -5);

  // check that child lock unwinds to parent lock

  {
    ItemCache proxy_child(proxy);

    ASSERT_TRUE(proxy_child.compute_storage_lock_acquire());

    proxy_child.mark_locks_acquired();

    lock = proxy_child.compute_storage_lock_release();

    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(lock->previous_type, StorageLockType::kSetObjectTypeClass);
    EXPECT_EQ(lock->new_type, StorageLockType::kSetObjectTypeClass);
    EXPECT_FALSE(!!lock->new_typeclass);
    // must release subtraction of -5
    EXPECT_EQ(lock->marginal_extra_lock.nnint64_subtracted_amount(), 5);
  }
}

} // namespace hogd
