#include <gtest/gtest.h>

#include <limits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel = StorageCostModel(12000, 1, 1, 1, 1);

} // namespace

TEST(NNInt128LockTest, BasicUsage)
{
  NNInt128Lock lock(100);

  NNInt128 after(100, 10);
  auto status = lock.try_atomic_change_lock(nullptr, &after, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.kBaseItemCost);
}

TEST(NNInt128LockTest, AddOverflow)
{

  NNInt128Lock lock(1);
  NNInt128 after(1, std::numeric_limits<int128_t>::max());
  auto status = lock.try_atomic_change_lock(nullptr, &after, &kTestCostModel);
  EXPECT_FALSE(status.has_value());

  NNInt128 good_after(1, std::numeric_limits<int128_t>::max() - 1);
  status = lock.try_atomic_change_lock(nullptr, &good_after, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.kBaseItemCost);
}

TEST(NNInt128LockTest, SubOverflow)
{
  NNInt128Lock lock(10);
  NNInt128 after(10, -10);
  auto status = lock.try_atomic_change_lock(nullptr, &after, &kTestCostModel);
  EXPECT_TRUE(status.has_value());

  NNInt128 another_sub(10, -5);
  status = lock.try_atomic_change_lock(nullptr, &another_sub, &kTestCostModel);
  EXPECT_FALSE(status.has_value());

  // release the held lock
  EXPECT_TRUE(lock.try_atomic_change_lock(&after, nullptr, &kTestCostModel)
                  .has_value());

  // now the lock should be available
  status = lock.try_atomic_change_lock(nullptr, &another_sub, &kTestCostModel);
  EXPECT_TRUE(status.has_value());
}

TEST(NNInt128LockTest, AddAndSubNarrowing)
{
  NNInt128Lock lock(100);

  // once we acquire this lock, no further adds should be possible
  NNInt128 first(100, std::numeric_limits<int128_t>::max() - 100);

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &first, &kTestCostModel)
                  .has_value());

  NNInt128 second(100, -100);
  second.narrow(first);
  EXPECT_TRUE(lock.try_atomic_change_lock(&first, &second, &kTestCostModel)
                  .has_value());

  // we already have the lock for this one
  NNInt128 third(100, 100);
  third.narrow(second);
  EXPECT_TRUE(lock.try_atomic_change_lock(&second, &third, &kTestCostModel)
                  .has_value());

  // we don't have the lock for more subtractions
  NNInt128 fourth(100, -101);
  EXPECT_FALSE(lock.try_atomic_change_lock(&third, &fourth, &kTestCostModel)
                   .has_value());

  // Concurrent txs can't add or sub
  NNInt128 othertx1_first(100, 100);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &othertx1_first, &kTestCostModel)
          .has_value());

  NNInt128 othertx2_first(100, -100);
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &othertx2_first, &kTestCostModel)
          .has_value());

  // If we release the lock, the others can succeed
  EXPECT_TRUE(lock.try_atomic_change_lock(&third, nullptr, &kTestCostModel)
                  .has_value());

  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &othertx1_first, &kTestCostModel)
          .has_value());
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &othertx2_first, &kTestCostModel)
          .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 200);
  EXPECT_EQ(lock.get_sub_capacity(), 0);
}

TEST(NNInt128LockTest, AddAndSubAcquireAndRelease)
{

  NNInt128Lock lock(100);

  const NNInt128 add(100, 100);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &add, &kTestCostModel).has_value());

  NNInt128 sub(100, -100);
  ASSERT_TRUE(sub.narrow(add));
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&add, &sub, &kTestCostModel).has_value());

  EXPECT_TRUE(
      lock.try_atomic_change_lock(&sub, nullptr, &kTestCostModel).has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 100);
  EXPECT_EQ(lock.get_sub_capacity(), 100);
}

TEST(NNInt128LockTest, HalfAcquireLock)
{

  NNInt128Lock lock(100);

  NNInt128 too_much_add(100, std::numeric_limits<int128_t>::max() - 99 - 50);

  // workaround: NNInt128 is considered invalid if base + delta < 0, or if base
  // + delta overflows
  NNInt128 ok_sub(100, -50);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &ok_sub, &kTestCostModel)
                  .has_value());
  NNInt128 ok_add(100, 50);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &ok_add, &kTestCostModel)
                  .has_value());

  NNInt128 too_much_sub(100, -51);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &too_much_add, &kTestCostModel)
          .has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &too_much_sub, &kTestCostModel)
          .has_value());

  NNInt128 good_sub(100, -50);
  good_sub.narrow(too_much_add);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &good_sub, &kTestCostModel)
                   .has_value());

  NNInt128 good_add(100, 100);
  good_add.narrow(too_much_sub);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &good_add, &kTestCostModel)
                   .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 100 - 50);
  EXPECT_EQ(lock.get_sub_capacity(), 50);
}

TEST(NNInt128LockTest, MultipleAddsOnValue)
{
  NNInt128Lock lock(100);

  NNInt128 add(100, 100);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &add, &kTestCostModel).has_value());

  NNInt128 add2(100, 50);
  add2.narrow(add);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&add, &add2, &kTestCostModel).has_value());
  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 200);
  EXPECT_EQ(lock.get_sub_capacity(), 100);

  EXPECT_EQ(lock.commit_change(true, 0, add2, &kTestCostModel),
            kTestCostModel.kBaseItemCost);

  EXPECT_EQ(lock.get_base_and_delta(),
            (std::pair<int128_t, int128_t>(100, 50)));
}
} // namespace hogd
