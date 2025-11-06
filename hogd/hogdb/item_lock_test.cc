#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"

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

constexpr StorageCostModel kTestCostModel = StorageCostModel(12000, 1, 1, 1, 1);
} // namespace

TEST(ItemLockTest, BasicUsage)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);

  // conflicting type
  auto conflicting = make_nnint_change(50, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  auto r3 = lock.try_atomic_change_lock(nullptr, &conflicting, &kTestCostModel);
  EXPECT_FALSE(r3.has_value());

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel),
            kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCost)
{
  ItemLock lock(10000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  EXPECT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost - 10000);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  EXPECT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel),
            kTestCostModel.kBaseItemCost - 10000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, &kTestCostModel);
  EXPECT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(third, &kTestCostModel), 0);
  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCostReleaseNonPermamnentExclusive)
{
  ItemLock lock(kTestCostModel.kBaseItemCost +
                1000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel), -1000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, &kTestCostModel);
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), 0);

  EXPECT_EQ(lock.commit_change(third, &kTestCostModel), 0);
  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), 0);
}

TEST(ItemLockTest, NNIntChangeBase)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);

  auto second_compat = make_nnint_change(200, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  ASSERT_FALSE(r2.has_value());

  r2 = lock.try_atomic_change_lock(&first, &second, &kTestCostModel);
  ASSERT_FALSE(r2.has_value());
  second.narrow(first);
  ASSERT_TRUE(second.is_exclusive());
  r2 = lock.try_atomic_change_lock(&first, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second_compat, &kTestCostModel)
          .has_value());

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel),
            kTestCostModel.kBaseItemCost);

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &second_compat, &kTestCostModel);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(second_compat, &kTestCostModel), 0);
}

TEST(ItemLockTest, NNIntChangeExclusiveAfterCommit)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto first2 = make_nnint_change(100, 20);
  auto second = make_nnint_change(200, 20);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  EXPECT_EQ(lock.commit_change(first, &kTestCostModel),
            kTestCostModel.kBaseItemCost);

  // Now we can acquire a shared lock but not an exclusive lock

  ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel)
                   .has_value());

  auto r2 = lock.try_atomic_change_lock(nullptr, &first2, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  EXPECT_FALSE(lock.try_atomic_change_lock(&first2, &second, &kTestCostModel)
                   .has_value());

  ItemChange first2_copy = first2;
  first2.narrow(second);

  ASSERT_TRUE(first2.is_exclusive());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&first2_copy, &first2, &kTestCostModel)
          .has_value());
}

TEST(ItemLockTest, ChangeItemType)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_bytestring_change({1, 2, 3});

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel)
                   .has_value());

  second.narrow(first);
  ASSERT_TRUE(second.is_exclusive());
  ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel)
                   .has_value());
  auto r2 = lock.try_atomic_change_lock(&first, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_bytestring_cost(3));

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel),
            kTestCostModel.get_bytestring_cost(3));
}

TEST(ItemLockTest, SeveralBytestringAcquires)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_bytestring_change({1, 2, 3});                 // 3 bytes
  auto second = make_bytestring_change({4, 5, 6, 7, 8});          // 5 bytes
  auto third = make_bytestring_change({7, 8, 9, 10, 11, 12, 13}); // 7 bytes

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_bytestring_cost(3));

  second.narrow(first);
  ASSERT_TRUE(second.is_exclusive());
  auto r2 = lock.try_atomic_change_lock(&first, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_bytestring_cost(5));

  third.narrow(second);
  auto r3 = lock.try_atomic_change_lock(&second, &third, &kTestCostModel);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_bytestring_cost(7));

  EXPECT_EQ(lock.commit_change(third, &kTestCostModel),
            kTestCostModel.get_bytestring_cost(7));

  // same as third
  auto post_commit = make_bytestring_change({7, 8, 9, 10, 11, 12, 13});
  auto r4 = lock.try_atomic_change_lock(nullptr, &post_commit, &kTestCostModel);
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.get_bytestring_cost(7));
  EXPECT_EQ(lock.commit_change(post_commit, &kTestCostModel), 0);

  // can't acaquire exclusive lock now
  auto post_commit_exclusive = post_commit;
  post_commit_exclusive.narrow(first);
  ASSERT_TRUE(post_commit_exclusive.is_exclusive());
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &post_commit, &kTestCostModel)
          .has_value());
  ASSERT_FALSE(lock.try_atomic_change_lock(&post_commit, &post_commit_exclusive,
                                           &kTestCostModel)
                   .has_value());
}

TEST(ItemLockTest, PermamentExclusiveInitial)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), -1000);
}

TEST(ItemLockTest, PermamentExclusiveDowngrade)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();
  auto second = make_nnint_change(200, 20);
  second.narrow(first);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  auto r2 = lock.try_atomic_change_lock(&first, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel), -1000);
}

TEST(ItemLockTest, PermamentExclusiveUpgradeDowngrade)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);
  second.narrow(first);
  auto third = second;
  second.set_permanent_exclusive();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(&first, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), -1000);

  auto r3 = lock.try_atomic_change_lock(&second, &third, &kTestCostModel);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), 0);

  EXPECT_EQ(lock.commit_change(third, &kTestCostModel), -1000);
}

TEST(ItemLockTest, PermamentExclusiveKeepsLock)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();
  auto second = make_nnint_change(100, 20);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel)
                   .has_value());

  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), -1000);

  // second should not conflict with first, except that first is permanent
  // exclusive
  ASSERT_FALSE(lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel)
                   .has_value());
}

TEST(ItemLockTest, DeletionChange)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_deletion_change();
  auto second = make_deletion_change();
  auto third = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  r1 = lock.try_atomic_change_lock(&first, &third, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel), 0);
  EXPECT_EQ(lock.commit_change(third, &kTestCostModel), 0);
}

TEST(ItemLockTest, DeletionChangeReleaseCost)
{
  ItemLock lock(100); // creating item from empty storage slot
  auto first = make_deletion_change();
  auto second = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, &kTestCostModel), -100);
  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), 0);
}

TEST(ItemLockTest, DeletionChangePermamentExclusiveRelease)
{
  ItemLock lock(100);
  auto first = make_deletion_change();
  first.set_permanent_exclusive();
  auto second = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -100);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  ASSERT_FALSE(r2.has_value());

  EXPECT_EQ(lock.commit_change(first, &kTestCostModel), -100);
}
} // namespace hogd
