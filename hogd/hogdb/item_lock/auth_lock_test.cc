#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/auth_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel = StorageCostModel(12000, 1, 1, 1, 1);

// Helper to create a PublicKey
PublicKey
make_test_key(uint8_t value)
{
  PublicKey key;
  key.ed25519().fill(value);
  return key;
}

// Helper to create a PublicKeyConfig
PublicKeyConfigOpt
make_test_pk_config(uint8_t value, Weight weight = 100)
{
  PublicKey key = make_test_key(value);
  return x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = key, .max_weight = weight});
}

// Helper to create an AccountAuthData with some keys
AccountAuthData
make_base_auth_data(std::vector<uint8_t> const &key_ids = {})
{
  AccountAuthData auth_data;
  for (size_t i = 0; i < MAX_SIGNERS; ++i) {
    auth_data.public_keys[i] = nullptr;
  }
  for (uint8_t id : key_ids) {
    if (id < MAX_SIGNERS) {
      auth_data.public_keys[id] = make_test_pk_config(id);
    }
  }
  return auth_data;
}

// Helper to create an AuthDataItem with specific key modifications
PointerItem<AuthDataItem>
make_auth_item(
    AccountAuthData base,
    std::vector<std::pair<uint32_t, PublicKeyConfigOpt>> const &changes)
{
  PointerItem<AuthDataItem> item(std::move(base));
  for (const auto &[key_id, pk] : changes) {
    item.get()->set_new_pk(key_id, pk);
  }
  return item;
}

// Overload that creates with empty base
PointerItem<AuthDataItem>
make_auth_item(
    std::vector<std::pair<uint32_t, PublicKeyConfigOpt>> const &changes)
{
  return make_auth_item(make_base_auth_data(), changes);
}

} // namespace

TEST(AuthLockTest, BasicAcquireAndRelease)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Acquire lock on key 0
  auto item1 = make_auth_item({{0, make_test_pk_config(1)}});
  auto status = lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(1));

  // Release lock
  status = lock.try_atomic_change_lock(&item1, nullptr, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
}

TEST(AuthLockTest, MultipleKeysSameTransaction)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Acquire locks on keys 0, 1, 2
  auto item1 = make_auth_item({{0, make_test_pk_config(1)},
                               {1, make_test_pk_config(2)},
                               {2, make_test_pk_config(3)}});
  auto status = lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(3));

  // Release all locks
  status = lock.try_atomic_change_lock(&item1, nullptr, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
}

TEST(AuthLockTest, ConcurrentNonConflicting)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 acquires lock on key 0
  auto tx1_item = make_auth_item({{0, make_test_pk_config(1)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 acquires lock on key 1 (different key, no conflict)
  auto tx2_item = make_auth_item({{1, make_test_pk_config(2)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());

  // Both can release
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx1_item, nullptr, &kTestCostModel)
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx2_item, nullptr, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, ConcurrentSameKeySameValue)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Tx1 acquires lock on key 0
  auto tx1_item = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 tries to acquire lock on same key with same value (should succeed)
  auto tx2_item = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());

  // Both release
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx1_item, nullptr, &kTestCostModel)
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx2_item, nullptr, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, ConcurrentSameKeyDifferentValue)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 acquires lock on key 0
  auto tx1_item = make_auth_item({{0, make_test_pk_config(1)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 tries to acquire lock on same key with different value (should fail)
  auto tx2_item = make_auth_item({{0, make_test_pk_config(2)}});
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                   .has_value());

  // Tx1 releases
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx1_item, nullptr, &kTestCostModel)
                  .has_value());

  // Now Tx2 can acquire
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, ExclusiveLockBasic)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Acquire exclusive lock on key 0
  auto item1 = make_auth_item({{0, make_test_pk_config(1)}});
  auto item1_narrow = make_auth_item({{0, make_test_pk_config(2)}});
  item1.narrow(item1_narrow);
  auto status = lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel);
  EXPECT_TRUE(status.has_value());

  // Another tx cannot acquire any lock on key 0
  auto item2 = make_auth_item({{0, make_test_pk_config(1)}});
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel)
                   .has_value());

  // Release exclusive lock
  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, nullptr, &kTestCostModel)
                  .has_value());

  // Now the other tx can acquire
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, ExclusiveLockBlocksOthers)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 acquires non-exclusive lock
  auto tx1_item = make_auth_item({{0, make_test_pk_config(1)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 tries to acquire exclusive lock (should fail - someone else holds lock)
  auto tx2_item = make_auth_item({{0, make_test_pk_config(1)}});
  auto tx2_item_narrow = make_auth_item({{0, make_test_pk_config(2)}});
  tx2_item.narrow(tx2_item_narrow);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                   .has_value());

  // Tx1 releases
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx1_item, nullptr, &kTestCostModel)
                  .has_value());

  // Now Tx2 can acquire exclusive lock
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, UpgradeToExclusive)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire non-exclusive lock
  auto item1 = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel)
                  .has_value());

  // Upgrade to exclusive (should succeed since we're the only one)
  auto item2 = make_auth_item({{0, make_test_pk_config(2)}});
  item2.narrow(item1);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&item1, &item2, &kTestCostModel).has_value());

  // Release
  EXPECT_TRUE(lock.try_atomic_change_lock(&item2, nullptr, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, CannotUpgradeToExclusiveWhenOthersHold)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Tx1 acquires non-exclusive lock
  auto tx1_item = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 acquires non-exclusive lock
  auto tx2_item = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());

  // Tx1 tries to upgrade to exclusive (should fail - Tx2 holds lock)
  auto tx1_exclusive = make_auth_item({{0, make_test_pk_config(2)}});
  tx1_exclusive.narrow(tx1_item);
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&tx1_item, &tx1_exclusive, &kTestCostModel)
          .has_value());
}

TEST(AuthLockTest, DowngradeFromExclusive)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire exclusive lock
  auto item1 = make_auth_item({{0, pk}});
  auto item1_narrow = make_auth_item({{0, make_test_pk_config(2)}});
  item1.narrow(item1_narrow);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel)
                  .has_value());

  // Downgrade to non-exclusive
  auto item2 = make_auth_item({{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&item1, &item2, &kTestCostModel).has_value());

  // Now another tx can acquire non-exclusive lock
  auto tx2_item = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, CommitSingleKey)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);
  auto item = make_auth_item({{0, pk}});

  // Acquire lock
  int128_t snapshot_cost = 0;
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel).has_value());

  // Commit (did_finalize = true)
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, &kTestCostModel);
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(1) - snapshot_cost);
}

TEST(AuthLockTest, CommitMultipleKeys)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto item = make_auth_item({{0, make_test_pk_config(1)},
                              {1, make_test_pk_config(2)},
                              {5, make_test_pk_config(3)}});

  // Acquire locks
  int128_t snapshot_cost = 0;
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel).has_value());

  // Commit
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, &kTestCostModel);
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(3) - snapshot_cost);
}

TEST(AuthLockTest, CommitWithBaseKeys)
{
  // Start with 2 keys already in base
  AccountAuthData base = make_base_auth_data({0, 1});
  AuthLock lock(base);

  // Add key 2
  auto item = make_auth_item({{2, make_test_pk_config(3)}});

  int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(2);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel).has_value());

  // Commit - should go from 2 keys to 3 keys
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, &kTestCostModel);
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(3) - snapshot_cost);
  EXPECT_EQ(commit_cost, kTestCostModel.kAuthPerKeyCost);
}

TEST(AuthLockTest, ExclusiveFlagClearedOnCommit)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire exclusive lock
  auto item1 = make_auth_item({{0, pk}});
  auto item1_narrow = make_auth_item({{0, make_test_pk_config(2)}});
  item1.narrow(item1_narrow);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel)
                  .has_value());

  // Commit (clears exclusive flag)
  lock.commit_change(true, 0, item1, &kTestCostModel);

  // Now another tx can acquire a lock (exclusive flag was cleared)
  auto item2 = make_auth_item({{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, StorageCostWithDeletedKey)
{
  AccountAuthData base = make_base_auth_data({0, 1, 2});
  AuthLock lock(base);

  // Delete key 1 (set to nullptr)
  auto item = make_auth_item({{1, nullptr}});

  // Reserve should still account for all possible keys
  auto status = lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
  // Base has 3 keys, we're not adding any (nullptr doesn't count)
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(3));

  // Commit the deletion
  int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(3);
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, &kTestCostModel);

  // After commit, we should have 2 keys (0, 2)
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(2) - snapshot_cost);
  EXPECT_EQ(commit_cost, -kTestCostModel.kAuthPerKeyCost);
}

TEST(AuthLockTest, PartialCommit)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 modifies key 0
  auto tx1_item = make_auth_item({{0, make_test_pk_config(1)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 modifies key 1
  auto tx2_item = make_auth_item({{1, make_test_pk_config(2)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                  .has_value());

  // Commit only tx1
  lock.commit_change(true, 0, tx1_item, &kTestCostModel);

  // Tx2 still holds its lock
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx2_item, nullptr, &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, MultipleKeysPartialOverlap)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Tx1 modifies keys 0, 1
  auto tx1_item = make_auth_item({{0, pk}, {1, make_test_pk_config(2)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1_item, &kTestCostModel)
                  .has_value());

  // Tx2 tries to modify keys 1, 2 (conflicts on key 1)
  auto tx2_item = make_auth_item(
      {{1, make_test_pk_config(3)}, {2, make_test_pk_config(4)}});
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &tx2_item, &kTestCostModel)
                   .has_value());

  // Tx2 can modify just key 2 (no conflict)
  auto tx2_item_no_conflict = make_auth_item({{2, make_test_pk_config(4)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item_no_conflict,
                                          &kTestCostModel)
                  .has_value());
}

TEST(AuthLockTest, KeepLockNoChange)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire lock
  auto item = make_auth_item({{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &item, &kTestCostModel).has_value());

  // "Upgrade" from item to same item (no change)
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&item, &item, &kTestCostModel).has_value());

  // Release
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&item, nullptr, &kTestCostModel).has_value());
}

TEST(AuthLockTest, ConcurrentExclusiveLocks)
{
  AccountAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto item1 = make_auth_item({{0, make_test_pk_config(1)}});
  auto item2 = make_auth_item({{1, make_test_pk_config(2)}});
  auto item1_narrow = make_auth_item({{0, make_test_pk_config(2)}});
  item1.narrow(item1_narrow);
  auto item2_narrow = make_auth_item({{1, make_test_pk_config(1)}});
  item2.narrow(item2_narrow);

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, &kTestCostModel)
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, &kTestCostModel)
                  .has_value());

  EXPECT_EQ(lock.commit_change(true, 0, item1, &kTestCostModel),
            kTestCostModel.get_auth_data_cost(1));
  EXPECT_EQ(lock.commit_change(false, 0, item2, &kTestCostModel),
            kTestCostModel.get_auth_data_cost(2) -
                kTestCostModel.get_auth_data_cost(1));
}

} // namespace hogd
