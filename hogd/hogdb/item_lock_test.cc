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
  EXPECT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  EXPECT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  auto r3 = lock.try_atomic_change_lock(nullptr, &conflicting, &kTestCostModel);
  EXPECT_FALSE(r3.has_value());

  EXPECT_EQ(lock.commit_change(&second, &kTestCostModel),
            kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(&first, &kTestCostModel), 0);
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

  EXPECT_EQ(lock.commit_change(&second, &kTestCostModel),
            kTestCostModel.kBaseItemCost - 10000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, &kTestCostModel);
  EXPECT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(&third, &kTestCostModel), 0);
  EXPECT_EQ(lock.commit_change(&first, &kTestCostModel), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCostReleaseNonPermamnentExclusive)
{
  ItemLock lock(kTestCostModel.kBaseItemCost +
                1000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel);
  EXPECT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, &kTestCostModel);
  EXPECT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(&second, &kTestCostModel), -1000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, &kTestCostModel);
  EXPECT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), 0);

  EXPECT_EQ(lock.commit_change(&third, &kTestCostModel), 0);
  EXPECT_EQ(lock.commit_change(&first, &kTestCostModel), 0);
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

  EXPECT_EQ(lock.commit_change(&second, &kTestCostModel),
            kTestCostModel.kBaseItemCost);

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &second_compat, &kTestCostModel);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(&second_compat, &kTestCostModel), 0);
}

} // namespace hogd
