#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/auth_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);

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

// Helper to create an AddressAuthData with some keys
AddressAuthData
make_base_auth_data(std::vector<uint8_t> const &key_ids = {})
{
  AddressAuthData auth_data{};
  for (size_t i = 0; i < kMaxSigners; ++i) {
    auth_data.public_keys[i] = nullptr;
  }
  for (uint8_t id : key_ids) {
    if (id < kMaxSigners) {
      auth_data.public_keys[id] = make_test_pk_config(id);
    }
  }
  return auth_data;
}

// Helper to create an AuthDataItem with specific key modifications
PointerItem<AuthDataItem>
make_auth_item(
    AddressAuthData base,
    std::vector<std::pair<uint32_t, PublicKeyConfigOpt>> const &changes)
{
  PointerItem<AuthDataItem> item(std::move(base));
  for (const auto &[key_id, pk] : changes) {
    item.get()->set_new_pk(key_id, pk);
  }
  return item;
}

// Helper to create a dummy LockContext for testing
inline LockContext
make_dummy_context()
{
  // These are never dereferenced in most lock implementations
  static HogDBDAO *dummy_dao = nullptr;
  static ItemKey dummy_key = make_item_key(make_address(0), 0);
  return LockContext(dummy_dao, 0, dummy_key, &kTestCostModel);
}

} // namespace

TEST(AuthLockTest, BasicAcquireAndRelease)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Acquire lock on key 0
  auto item1 = make_auth_item(base, {{0, make_test_pk_config(1)}});
  auto status =
      lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(1));

  // Release lock
  status = lock.try_atomic_change_lock(&item1, nullptr, make_dummy_context());
  EXPECT_TRUE(status.has_value());
}

TEST(AuthLockTest, MultipleKeysSameTransaction)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Acquire locks on keys 0, 1, 2
  auto item1 = make_auth_item(base, {{0, make_test_pk_config(1)},
                                     {1, make_test_pk_config(2)},
                                     {2, make_test_pk_config(3)}});
  auto status =
      lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(3));

  // Release all locks
  status = lock.try_atomic_change_lock(&item1, nullptr, make_dummy_context());
  EXPECT_TRUE(status.has_value());
}

TEST(AuthLockTest, DISABLED_ConcurrentNonConflicting)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 acquires lock on key 0
  auto tx1_item = make_auth_item(base, {{0, make_test_pk_config(1)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 acquires lock on key 1 (different key, no conflict)
  auto tx2_item = make_auth_item(base, {{1, make_test_pk_config(2)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Both can release
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx1_item, nullptr, make_dummy_context())
          .has_value());
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx2_item, nullptr, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, DISABLED_ConcurrentSameKeySameValue)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Tx1 acquires lock on key 0
  auto tx1_item = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 tries to acquire lock on same key with same value (should succeed)
  auto tx2_item = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Both release
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx1_item, nullptr, make_dummy_context())
          .has_value());
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx2_item, nullptr, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, DISABLED_ConcurrentSameKeyDifferentValue)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 acquires lock on key 0
  auto tx1_item = make_auth_item(base, {{0, make_test_pk_config(1)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 tries to acquire lock on same key with different value (should fail)
  auto tx2_item = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Tx1 releases
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx1_item, nullptr, make_dummy_context())
          .has_value());

  // Now Tx2 can acquire
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, DISABLED_ExclusiveLockBasic)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Acquire exclusive lock on key 0
  auto item1 = make_auth_item(base, {{0, make_test_pk_config(1)}});
  auto item1_narrow = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(item1.narrow(item1_narrow).has_value());
  auto status =
      lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context());
  EXPECT_TRUE(status.has_value());

  // Another tx cannot acquire any lock on key 0
  auto item2 = make_auth_item(base, {{0, make_test_pk_config(1)}});
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &item2, make_dummy_context())
          .has_value());

  // Release exclusive lock
  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, nullptr, make_dummy_context())
                  .has_value());

  // Now the other tx can acquire
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, make_dummy_context())
                  .has_value());
}

TEST(AuthLockTest, DISABLED_ExclusiveLockBlocksOthers)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 acquires non-exclusive lock
  auto tx1_item = make_auth_item(base, {{0, make_test_pk_config(1)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 tries to acquire exclusive lock (should fail - someone else holds lock)
  auto tx2_item = make_auth_item(base, {{0, make_test_pk_config(1)}});
  auto tx2_item_narrow = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(tx2_item.narrow(tx2_item_narrow).has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Tx1 releases
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx1_item, nullptr, make_dummy_context())
          .has_value());

  // Now Tx2 can acquire exclusive lock
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, DISABLED_UpgradeToExclusive)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire non-exclusive lock
  auto item1 = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());

  // Upgrade to exclusive (should succeed since we're the only one)
  auto item2 = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(item2.narrow(item1).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, &item2, make_dummy_context())
                  .has_value());

  // Release
  EXPECT_TRUE(lock.try_atomic_change_lock(&item2, nullptr, make_dummy_context())
                  .has_value());
}

TEST(AuthLockTest, DISABLED_CannotUpgradeToExclusiveWhenOthersHold)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Tx1 acquires non-exclusive lock
  auto tx1_item = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 acquires non-exclusive lock
  auto tx2_item = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Tx1 tries to upgrade to exclusive (should fail - Tx2 holds lock)
  auto tx1_exclusive = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(tx1_exclusive.narrow(tx1_item).has_value());
  EXPECT_FALSE(lock.try_atomic_change_lock(&tx1_item, &tx1_exclusive,
                                           make_dummy_context())
                   .has_value());
}

TEST(AuthLockTest, DISABLED_DowngradeFromExclusive)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire exclusive lock
  auto item1 = make_auth_item(base, {{0, pk}});
  auto item1_narrow = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(item1.narrow(item1_narrow).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());

  // Downgrade to non-exclusive
  auto item2 = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, &item2, make_dummy_context())
                  .has_value());

  // Now another tx can acquire non-exclusive lock
  auto tx2_item = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, CommitSingleKey)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);
  auto item = make_auth_item(base, {{0, pk}});

  // Acquire lock
  int128_t snapshot_cost = 0;
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item, make_dummy_context())
                  .has_value());

  // Commit (did_finalize = true)
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, make_dummy_context());
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(1) - snapshot_cost);
}

TEST(AuthLockTest, CommitMultipleKeys)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto item = make_auth_item(base, {{0, make_test_pk_config(1)},
                                    {1, make_test_pk_config(2)},
                                    {5, make_test_pk_config(3)}});

  // Acquire locks
  int128_t snapshot_cost = 0;
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item, make_dummy_context())
                  .has_value());

  // Commit
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, make_dummy_context());
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(3) - snapshot_cost);
}

TEST(AuthLockTest, CommitWithBaseKeys)
{
  // Start with 2 keys already in base
  AddressAuthData base = make_base_auth_data({0, 1});
  AuthLock lock(base);

  // Add key 2
  auto item = make_auth_item(base, {{2, make_test_pk_config(3)}});

  int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(2);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item, make_dummy_context())
                  .has_value());

  // Commit - should go from 2 keys to 3 keys
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, make_dummy_context());
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(3) - snapshot_cost);
  EXPECT_EQ(commit_cost, kTestCostModel.kAuthPerKeyCost);
}

TEST(AuthLockTest, DISABLED_ExclusiveFlagClearedOnCommit)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire exclusive lock
  auto item1 = make_auth_item(base, {{0, pk}});
  auto item1_narrow = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(item1.narrow(item1_narrow).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());

  // Commit (clears exclusive flag)
  lock.commit_change(true, 0, item1, make_dummy_context());

  // Now another tx can acquire a lock (exclusive flag was cleared)
  auto item2 = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, make_dummy_context())
                  .has_value());
}

TEST(AuthLockTest, StorageCostWithDeletedKey)
{
  AddressAuthData base = make_base_auth_data({0, 1, 2});
  AuthLock lock(base);

  // Delete key 1 (set to nullptr)
  auto item = make_auth_item(base, {{1, nullptr}});

  // Reserve should still account for all possible keys
  auto status =
      lock.try_atomic_change_lock(nullptr, &item, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  // Base has 3 keys, we're not adding any (nullptr doesn't count)
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(3));

  // Commit the deletion
  int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(3);
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, make_dummy_context());

  // After commit, we should have 2 keys (0, 2)
  EXPECT_EQ(commit_cost, kTestCostModel.get_auth_data_cost(2) - snapshot_cost);
  EXPECT_EQ(commit_cost, -int128_t{kTestCostModel.kAuthPerKeyCost});
}

TEST(AuthLockTest, DISABLED_PartialCommit)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx1 modifies key 0
  auto tx1_item = make_auth_item(base, {{0, make_test_pk_config(1)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 modifies key 1
  auto tx2_item = make_auth_item(base, {{1, make_test_pk_config(2)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Commit only tx1
  lock.commit_change(true, 0, tx1_item, make_dummy_context());

  // Tx2 still holds its lock
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&tx2_item, nullptr, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, DISABLED_MultipleKeysPartialOverlap)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Tx1 modifies keys 0, 1
  auto tx1_item = make_auth_item(base, {{0, pk}, {1, make_test_pk_config(2)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &tx1_item, make_dummy_context())
          .has_value());

  // Tx2 tries to modify keys 1, 2 (conflicts on key 1)
  auto tx2_item = make_auth_item(base, {{1, make_test_pk_config(3)},
                                        {2, make_test_pk_config(4)}});
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &tx2_item, make_dummy_context())
          .has_value());

  // Tx2 can modify just key 2 (no conflict)
  auto tx2_item_no_conflict =
      make_auth_item(base, {{2, make_test_pk_config(4)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2_item_no_conflict,
                                          make_dummy_context())
                  .has_value());
}

TEST(AuthLockTest, DISABLED_KeepLockNoChange)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Acquire lock
  auto item = make_auth_item(base, {{0, pk}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item, make_dummy_context())
                  .has_value());

  // "Upgrade" from item to same item (no change)
  EXPECT_TRUE(lock.try_atomic_change_lock(&item, &item, make_dummy_context())
                  .has_value());

  // Release
  EXPECT_TRUE(lock.try_atomic_change_lock(&item, nullptr, make_dummy_context())
                  .has_value());
}

TEST(AuthLockTest, DISABLED_ConcurrentExclusiveLocks)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto item1 = make_auth_item(base, {{0, make_test_pk_config(1)}});
  auto item2 = make_auth_item(base, {{1, make_test_pk_config(2)}});
  auto item1_narrow = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(item1.narrow(item1_narrow).has_value());
  auto item2_narrow = make_auth_item(base, {{1, make_test_pk_config(1)}});
  EXPECT_TRUE(item2.narrow(item2_narrow).has_value());

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item2, make_dummy_context())
                  .has_value());

  EXPECT_EQ(lock.commit_change(true, 0, item1, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
  EXPECT_EQ(lock.commit_change(false, 0, item2, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2) -
                kTestCostModel.get_auth_data_cost(1));
}

TEST(AuthLockTest, AcquireCosts1)
{
  // start with 3 keys in base
  const AddressAuthData base = make_base_auth_data({0, 1, 2});
  AuthLock lock(base);

  auto item1 =
      make_auth_item(base,
                     {{0, make_test_pk_config(1)}, {1, nullptr}, {4, nullptr}});

  auto r1 = lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(3));

  // doesn't pay for the nullptr on 4
  EXPECT_EQ(lock.commit_change(true, 0, item1, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2));
}

TEST(AuthLockTest, DISABLED_AcquireCosts2)
{
  // start with 1 key in base
  const AddressAuthData base = make_base_auth_data({0});
  const int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(1);
  AuthLock lock(base);

  // We can't refund deleting keys in auth data because we don't pass the
  // permanent exclusive lock flag into the auth data item. These two changes
  // are both valid, but if committed together with a tx-local refund would have
  // the possibility of double-refunding the deleted key.

  auto item1 =
      make_auth_item(base, {{0, nullptr}, {1, make_test_pk_config(2)}});
  auto item2 =
      make_auth_item(base, {{0, nullptr}, {2, make_test_pk_config(3)}});

  auto r1 = lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  // pay for new key on 1, doesn't get refund for deleting 0
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  auto r2 = lock.try_atomic_change_lock(nullptr, &item2, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  // pay for new key on 2, doesn't get refund for deleting 0
  EXPECT_EQ(r2.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(true, snapshot_cost, item1,
                               make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1) - snapshot_cost);
  EXPECT_EQ(lock.commit_change(false, snapshot_cost, item2,
                               make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2) -
                kTestCostModel.get_auth_data_cost(1));
}

TEST(AuthLockTest, DISABLED_SwapKeys)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx acquires key 0
  auto item1 = make_auth_item(base, {{0, make_test_pk_config(1)}});
  // conflicts with item1 on key 0
  auto other_tx = make_auth_item(base, {{0, make_test_pk_config(5)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &other_tx, make_dummy_context())
          .has_value());

  // Tx changes to release key 0 and acquire key 1 instead
  auto item2 = make_auth_item(base, {{1, make_test_pk_config(2)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, &item2, make_dummy_context())
                  .has_value());

  // Now another tx can acquire key 0 (was released)
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &other_tx, make_dummy_context())
          .has_value());
}

TEST(AuthLockTest, DISABLED_ThreeConcurrentTransactionsSameKey)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  auto pk = make_test_pk_config(1);

  // Three transactions all acquire locks on same key with same value
  auto tx1 = make_auth_item(base, {{0, pk}});
  auto tx2 = make_auth_item(base, {{0, pk}});
  auto tx3 = make_auth_item(base, {{0, pk}});

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx1, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx2, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &tx3, make_dummy_context())
                  .has_value());

  // None can upgrade to exclusive
  auto tx1_excl = make_auth_item(base, {{0, make_test_pk_config(2)}});
  EXPECT_TRUE(tx1_excl.narrow(tx1).has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&tx1, &tx1_excl, make_dummy_context())
          .has_value());

  // Release tx2 and tx3
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx2, nullptr, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx3, nullptr, make_dummy_context())
                  .has_value());

  // Now tx3 can upgrade to exclusive
  EXPECT_TRUE(lock.try_atomic_change_lock(&tx1, &tx1_excl, make_dummy_context())
                  .has_value());
}

TEST(AuthLockTest, SetRequiredWeight)
{
  AddressAuthData base = make_base_auth_data({0, 1});
  AuthLock lock(base);

  // Create an item that sets required weight
  PointerItem<AuthDataItem> item(base);
  item.get()->set_new_required_weight(Weight{2});

  // Acquire lock
  int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(2);
  auto status =
      lock.try_atomic_change_lock(nullptr, &item, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(2));

  // Commit the change
  int128_t commit_cost =
      lock.commit_change(true, snapshot_cost, item, make_dummy_context());
  EXPECT_EQ(commit_cost, 0); // No key count change, so no cost change

  // Verify the required weight was set
  AddressAuthData result = lock.build_result(/*block_height=*/0);
  EXPECT_EQ(result.required_weight, Weight{2});
}

TEST(AuthLockTest, RequiredWeightRollback)
{
  AddressAuthData base = make_base_auth_data({0, 1});
  AuthLock lock(base);

  // First change: set required weight
  PointerItem<AuthDataItem> item1(base);
  item1.get()->set_new_required_weight(Weight{1});

  int128_t snapshot_cost = kTestCostModel.get_auth_data_cost(2);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());

  // Second change: change required weight (this is the "middle" state we'll
  // rollback to)
  PointerItem<AuthDataItem> item2(base);
  item2.get()->set_new_required_weight(Weight{2});

  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, &item2, make_dummy_context())
                  .has_value());

  // Third change: change required weight again
  PointerItem<AuthDataItem> item3(base);
  item3.get()->set_new_required_weight(Weight{3});

  EXPECT_TRUE(lock.try_atomic_change_lock(&item2, &item3, make_dummy_context())
                  .has_value());

  // Rollback: transition from item3 back to item2
  EXPECT_TRUE(lock.try_atomic_change_lock(&item3, &item2, make_dummy_context())
                  .has_value());

  // Commit the rollback (item2)
  lock.commit_change(false, snapshot_cost, item2, make_dummy_context());

  // Verify final state matches item2 (the middle state)
  AddressAuthData result = lock.build_result(/*block_height=*/0);
  EXPECT_EQ(result.required_weight, Weight{2});
}

TEST(AuthLockTest, AllKeysUsed)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Create a transaction that modifies all MAX_SIGNERS keys
  std::vector<std::pair<uint32_t, PublicKeyConfigOpt>> all_keys;
  all_keys.reserve(kMaxSigners);
  for (uint32_t i = 0; i < kMaxSigners; ++i) {
    all_keys.emplace_back(i, make_test_pk_config(static_cast<uint8_t>(i)));
  }

  auto item = make_auth_item(base, all_keys);
  auto status =
      lock.try_atomic_change_lock(nullptr, &item, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.get_auth_data_cost(kMaxSigners));

  // Commit
  int128_t cost = lock.commit_change(true, 0, item, make_dummy_context());
  EXPECT_EQ(cost, kTestCostModel.get_auth_data_cost(kMaxSigners));
}

TEST(AuthLockTest, DISABLED_PartialRelease)
{
  AddressAuthData base = make_base_auth_data();
  AuthLock lock(base);

  // Tx acquires keys 0, 1, 2
  auto item1 = make_auth_item(base, {{0, make_test_pk_config(1)},
                                     {1, make_test_pk_config(2)},
                                     {2, make_test_pk_config(3)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());

  // Tx releases keys 0 and 2, keeps key 1
  auto item2 = make_auth_item(base, {{1, make_test_pk_config(2)}});
  EXPECT_TRUE(lock.try_atomic_change_lock(&item1, &item2, make_dummy_context())
                  .has_value());

  // Another tx can now acquire key 0
  auto other_tx1 = make_auth_item(base, {{0, make_test_pk_config(1)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &other_tx1, make_dummy_context())
          .has_value());

  // And key 2
  auto other_tx2 = make_auth_item(base, {{2, make_test_pk_config(3)}});
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &other_tx2, make_dummy_context())
          .has_value());

  // But not key 1 with different value
  auto conflicting_tx = make_auth_item(base, {{1, make_test_pk_config(99)}});
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &conflicting_tx,
                                           make_dummy_context())
                   .has_value());
}

} // namespace hogd
