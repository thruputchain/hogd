#include <gtest/gtest.h>

#include <atomic>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

namespace {
ItemChange
make_nnint_change(int128_t set, int128_t add)
{
  return NNInt128(set, add);
}

ItemChange
make_bytestring_change(std::vector<uint8_t> data)
{
  return Bytes(std::as_bytes(std::span(data)));
}

ItemChange
make_deletion_change()
{
  return Deletion();
}

AddressAuthData
make_base_auth_data(
    std::unordered_map<uint32_t, PublicKeyConfigOpt> public_keys)
{
  AddressAuthData base_auth_data{};
  for (auto &[key_id, pk] : public_keys) {
    base_auth_data.public_keys[key_id] = std::move(pk);
  }
  return base_auth_data;
}

AuthDataItem
make_auth_data_change(
    std::unordered_map<uint32_t, PublicKeyConfigOpt> const &public_keys)
{
  return AuthDataItem(make_base_auth_data(public_keys));
}

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

constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);

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

TEST(ItemLockTest, BasicUsage)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);

  // conflicting type
  auto conflicting = make_nnint_change(50, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &conflicting, make_dummy_context());
  EXPECT_FALSE(r3.has_value());

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCost)
{
  ItemLock lock(10000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  EXPECT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost - 10000);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  EXPECT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost - 10000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, make_dummy_context());
  EXPECT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCostReleaseNonPermamnentExclusive)
{
  ItemLock lock(kTestCostModel.kBaseItemCost +
                1000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), -1000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, make_dummy_context());
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), 0);

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, NNIntChangeBase)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);

  auto second_compat = make_nnint_change(200, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());

  r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());
  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());
  r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second_compat, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  auto r3 = lock.try_atomic_change_lock(nullptr, &second_compat,
                                        make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(second_compat, make_dummy_context()), 0);
}

TEST(ItemLockTest, NNIntChangeExclusiveAfterCommit)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto first2 = make_nnint_change(100, 20);
  auto second = make_nnint_change(200, 20);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  // Now we can acquire a shared lock but not an exclusive lock

  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());

  auto r2 = lock.try_atomic_change_lock(nullptr, &first2, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(&first2, &second, make_dummy_context())
          .has_value());

  ItemChange first2_copy = first2;
  EXPECT_TRUE(first2.narrow(second).has_value());

  ASSERT_TRUE(first2.is_exclusive());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&first2_copy, &first2, make_dummy_context())
          .has_value());
}

TEST(ItemLockTest, ChangeItemType)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_bytestring_change({1, 2, 3});

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());

  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());
  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_bytestring_cost(3));

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.get_bytestring_cost(3));
}

TEST(ItemLockTest, SeveralBytestringAcquires)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_bytestring_change({1, 2, 3});                 // 3 bytes
  auto second = make_bytestring_change({4, 5, 6, 7, 8});          // 5 bytes
  auto third = make_bytestring_change({7, 8, 9, 10, 11, 12, 13}); // 7 bytes

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_bytestring_cost(3));

  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());
  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_bytestring_cost(5));

  EXPECT_TRUE(third.narrow(second).has_value());
  auto r3 = lock.try_atomic_change_lock(&second, &third, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_bytestring_cost(7));

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()),
            kTestCostModel.get_bytestring_cost(7));

  // same as third
  auto post_commit = make_bytestring_change({7, 8, 9, 10, 11, 12, 13});
  auto r4 =
      lock.try_atomic_change_lock(nullptr, &post_commit, make_dummy_context());
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.get_bytestring_cost(7));
  EXPECT_EQ(lock.commit_change(post_commit, make_dummy_context()), 0);

  // can't acaquire exclusive lock now
  auto post_commit_exclusive = post_commit;
  EXPECT_TRUE(post_commit_exclusive.narrow(first).has_value());
  ASSERT_TRUE(post_commit_exclusive.is_exclusive());
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &post_commit, make_dummy_context())
          .has_value());
  ASSERT_FALSE(lock.try_atomic_change_lock(&post_commit, &post_commit_exclusive,
                                           make_dummy_context())
                   .has_value());
}

TEST(ItemLockTest, PermamentExclusiveInitial)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), -1000);
}

TEST(ItemLockTest, PermamentExclusiveDowngrade)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), -1000);
}

TEST(ItemLockTest, PermamentExclusiveUpgradeDowngrade)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());
  auto third = second;
  second.set_permanent_exclusive();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), -1000);

  auto r3 = lock.try_atomic_change_lock(&second, &third, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), 0);

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), -1000);
}

TEST(ItemLockTest, PermamentExclusiveKeepsLock)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();
  auto second = make_nnint_change(100, 20);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), -1000);

  // second should not conflict with first, except that first is permanent
  // exclusive
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());
}

TEST(ItemLockTest, DeletionChange)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_deletion_change();
  auto second = make_deletion_change();
  auto third = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  r1 = lock.try_atomic_change_lock(&first, &third, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), 0);
}

TEST(ItemLockTest, DeletionChangeReleaseCost)
{
  ItemLock lock(100); // creating item from empty storage slot
  auto first = make_deletion_change();
  auto second = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), -100);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, DeletionChangePermamentExclusiveRelease)
{
  ItemLock lock(100);
  auto first = make_deletion_change();
  first.set_permanent_exclusive();
  auto second = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -100);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), -100);
}

TEST(ItemLockTest, BaseDataConflict)
{
  // In practice, auth data will always have the base data be from the previous
  // snapshot, so it will always be the same. Still, worth checking to make sure
  // we don't have some bizarre errors there.
  ItemLock lock(0);
  auto first = ItemChange(make_auth_data_change({{0, make_test_pk_config(1)}}));
  auto second =
      ItemChange(make_auth_data_change({{1, make_test_pk_config(2)}}));

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(1));

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());

  ASSERT_FALSE(
      lock.try_atomic_change_lock(&first, &second, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
}

TEST(ItemLockTest, AuthAddKey)
{
  ItemLock lock(0);
  // start with 1 initial key
  auto first = make_auth_data_change({{0, make_test_pk_config(1)}});
  first.set_new_pk(1, make_test_pk_config(2));
  ItemChange first_change(std::move(first));

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2));
}

TEST(ItemLockTest, ChangeKey)
{
  ItemLock lock(0);
  auto first = make_auth_data_change({{0, make_test_pk_config(1)}});
  first.set_new_pk(0, make_test_pk_config(2));
  ItemChange first_change(std::move(first));

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(1));

  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
}

TEST(ItemLockTest, DISABLED_AuthAddKeyMultiple)
{
  ItemLock lock(0);
  auto first = make_auth_data_change({{0, make_test_pk_config(1)}});
  first.set_new_pk(1, make_test_pk_config(2));
  ItemChange first_change(std::move(first));

  // NOLINTBEGIN(performance-unnecessary-copy-initialization)
  ItemChange second_change = first_change;
  ItemChange third_change = first_change;
  // NOLINTEND(performance-unnecessary-copy-initialization)

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  auto r2 = lock.try_atomic_change_lock(nullptr, &second_change,
                                        make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_auth_data_cost(2));

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &third_change, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(second_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2));
  EXPECT_EQ(lock.commit_change(third_change, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()), 0);
}

TEST(ItemLockTest, DISABLED_MoveKeys)
{
  // This test starts with 1 initial key.  Each transaction deletes that key and
  // creates another key elsewhere.
  auto pk = make_test_pk_config(1);
  ItemLock lock(0);
  auto first = make_auth_data_change({{0, pk}});
  auto second = make_auth_data_change({{0, pk}});
  auto third = make_auth_data_change({{0, pk}});

  first.set_new_pk(0, nullptr);
  second.set_new_pk(0, nullptr);
  third.set_new_pk(0, nullptr);

  first.set_new_pk(1, make_test_pk_config(2));
  second.set_new_pk(2, make_test_pk_config(2));
  third.set_new_pk(3, make_test_pk_config(2));

  ItemChange first_change(std::move(first));
  ItemChange second_change(std::move(second));
  ItemChange third_change(std::move(third));

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  auto r2 = lock.try_atomic_change_lock(nullptr, &second_change,
                                        make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_auth_data_cost(2));

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &third_change, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(second_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
  EXPECT_EQ(lock.commit_change(third_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2) -
                kTestCostModel.get_auth_data_cost(1));
  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(3) -
                kTestCostModel.get_auth_data_cost(2));
}

TEST(ItemLockTest, FinalizeExclusive)
{
  // Reproducing for clarity an error case from
  // TryInvokeTest.NNIntAddSimpleInterleavings
  ItemLock lock(0);
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  auto const *inner = lock.get_inner<NNInt128Lock>();
  ASSERT_TRUE(inner != nullptr);
  EXPECT_EQ(inner->get_base_and_delta(),
            (std::pair<int128_t, int128_t>(200, 20)));
}

TEST(ItemLockTest, FinalizedFastPathRelaxedLoadDataRace)
{
  for (int i = 0; i < 10000; ++i) {
    ItemLock lock(0);
    auto change_a = make_nnint_change(100, 10);
    auto change_b = make_nnint_change(100, 20);

    std::atomic<bool> finalized_ready = false;
    std::atomic<bool> committed = false;

    std::thread thread_a([&]() {
      auto ctx = make_dummy_context();
      auto res = lock.try_atomic_change_lock(nullptr, &change_a, ctx);
      if (res.has_value()) {
        committed.store(true, std::memory_order_relaxed);
        (void)lock.commit_change(change_a, ctx);
      }
      finalized_ready.store(true, std::memory_order_relaxed);
    });

    std::thread thread_b([&]() {
      while (!finalized_ready.load(std::memory_order_relaxed)) {
      }
      auto ctx = make_dummy_context();
      auto ignored = lock.try_atomic_change_lock(nullptr, &change_b, ctx);
      (void)ignored;
    });

    thread_a.join();
    thread_b.join();
    ASSERT_TRUE(committed.load(std::memory_order_relaxed));
  }
}

} // namespace hogd
