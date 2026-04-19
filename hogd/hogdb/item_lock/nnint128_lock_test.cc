#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <utility>

#include "hogd/base/io.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#define private public
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#undef private
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {
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

inline HogDBDAO &
test_dao()
{
  static std::optional<Directory> dir;
  static std::optional<HogDBDAO> dao;
  if (!dao.has_value()) {
    auto dir_status = Directory::open_temporary();
    CHECK(static_cast<bool>(dir_status));
    dir.emplace(std::move(*dir_status));

    constexpr size_t kHeapSize = 10_MiB;
    auto db = Database::open(dir->get_path(),
                             Database::estimate_heap_size(kHeapSize), 0);
    CHECK(static_cast<bool>(db));
    dao.emplace(RAMDB(std::move(*db)));
  }

  return *dao;
}

inline LockContext
make_persist_context()
{
  static ItemKey key = make_item_key(make_address(1), 0);
  return LockContext(&test_dao(), 0, key, &kTestCostModel);
}

} // namespace

TEST(NNInt128LockTest, BasicUsage)
{
  NNInt128Lock lock(100);

  NNInt128 after(100, 10);
  auto status =
      lock.try_atomic_change_lock(nullptr, &after, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.kBaseItemCost);
}

TEST(NNInt128LockTest, AddOverflow)
{

  NNInt128Lock lock(1);
  NNInt128 after(1, std::numeric_limits<int128_t>::max());
  auto status =
      lock.try_atomic_change_lock(nullptr, &after, make_dummy_context());
  EXPECT_FALSE(status.has_value());

  NNInt128 good_after(1, std::numeric_limits<int128_t>::max() - 1);
  status =
      lock.try_atomic_change_lock(nullptr, &good_after, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), kTestCostModel.kBaseItemCost);
}

TEST(NNInt128LockTest, SubOverflow)
{
  NNInt128Lock lock(10);
  NNInt128 after(10, -10);
  auto status =
      lock.try_atomic_change_lock(nullptr, &after, make_dummy_context());
  EXPECT_TRUE(status.has_value());

  NNInt128 another_sub(10, -5);
  status =
      lock.try_atomic_change_lock(nullptr, &another_sub, make_dummy_context());
  EXPECT_FALSE(status.has_value());

  // release the held lock
  EXPECT_TRUE(lock.try_atomic_change_lock(&after, nullptr, make_dummy_context())
                  .has_value());

  // now the lock should be available
  status =
      lock.try_atomic_change_lock(nullptr, &another_sub, make_dummy_context());
  EXPECT_TRUE(status.has_value());
}

TEST(NNInt128LockTest, AddAndSubNarrowing)
{
  NNInt128Lock lock(100);

  // once we acquire this lock, no further adds should be possible
  NNInt128 first(100, std::numeric_limits<int128_t>::max() - 100);

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &first, make_dummy_context())
                  .has_value());

  NNInt128 second(100, -100);
  EXPECT_TRUE(second.narrow(first).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&first, &second, make_dummy_context())
                  .has_value());

  // we already have the lock for this one
  NNInt128 third(100, 100);
  EXPECT_TRUE(third.narrow(second).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&second, &third, make_dummy_context())
                  .has_value());

  // we don't have the lock for more subtractions
  NNInt128 fourth(100, -101);
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&third, &fourth, make_dummy_context())
          .has_value());

  // Concurrent txs can't add or sub
  NNInt128 othertx1_first(100, 100);

  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &othertx1_first,
                                           make_dummy_context())
                   .has_value());

  NNInt128 othertx2_first(100, -100);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &othertx2_first,
                                           make_dummy_context())
                   .has_value());

  // If we release the lock, the others can succeed
  EXPECT_TRUE(lock.try_atomic_change_lock(&third, nullptr, make_dummy_context())
                  .has_value());

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &othertx1_first,
                                          make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &othertx2_first,
                                          make_dummy_context())
                  .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 200);
  EXPECT_EQ(lock.get_sub_capacity(), 0);
}

TEST(NNInt128LockTest, AddAndSubAcquireAndRelease)
{

  NNInt128Lock lock(100);

  const NNInt128 add(100, 100);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &add, make_dummy_context())
                  .has_value());

  NNInt128 sub(100, -100);
  ASSERT_TRUE(sub.narrow(add).value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&add, &sub, make_dummy_context())
                  .has_value());

  EXPECT_TRUE(lock.try_atomic_change_lock(&sub, nullptr, make_dummy_context())
                  .has_value());

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
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &ok_sub, make_dummy_context())
          .has_value());
  NNInt128 ok_add(100, 50);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &ok_add, make_dummy_context())
          .has_value());

  NNInt128 too_much_sub(100, -51);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &too_much_add, make_dummy_context())
          .has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &too_much_sub, make_dummy_context())
          .has_value());

  NNInt128 good_sub(100, -50);
  EXPECT_TRUE(good_sub.narrow(too_much_add).has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &good_sub, make_dummy_context())
          .has_value());

  NNInt128 good_add(100, 100);
  EXPECT_TRUE(good_add.narrow(too_much_sub).has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &good_add, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 100 - 50);
  EXPECT_EQ(lock.get_sub_capacity(), 50);
}

TEST(NNInt128LockTest, MultipleAddsOnValue)
{
  NNInt128Lock lock(100);

  NNInt128 add(100, 100);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &add, make_dummy_context())
                  .has_value());

  NNInt128 add2(100, 50);
  EXPECT_TRUE(add2.narrow(add).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&add, &add2, make_dummy_context())
                  .has_value());
  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 200);
  EXPECT_EQ(lock.get_sub_capacity(), 100);

  EXPECT_EQ(lock.commit_change(true, 0, add2, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  EXPECT_EQ(lock.get_base_and_delta(),
            (std::pair<int128_t, int128_t>(100, 50)));
}

TEST(NNInt128LockTest, NegativeBase)
{
  NNInt128Lock lock(-100);
  EXPECT_EQ(lock.get_add_capacity(),
            uint128_t{std::numeric_limits<int128_t>::max()} + 100);
  EXPECT_EQ(lock.get_sub_capacity(), 0);

  NNInt128 add(-100, 100);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &add, make_dummy_context())
                  .has_value());
  EXPECT_EQ(lock.get_add_capacity(),
            uint128_t{std::numeric_limits<int128_t>::max()});
  EXPECT_EQ(lock.get_sub_capacity(), 0);

  NNInt128 sub(-100, -100);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &sub, make_dummy_context())
                   .has_value());

  NNInt128 big_add(-100, std::numeric_limits<int128_t>::max());

  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &big_add, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(true, 0, big_add, make_dummy_context()),
            kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(false, 0, add, make_dummy_context()), 0);

  EXPECT_EQ(lock.get_base_and_delta(),
            (std::pair<int128_t, int128_t>(
                -100, uint128_t{std::numeric_limits<int128_t>::max()} + 100)));
  EXPECT_EQ(lock.get_add_capacity(), 0);
  EXPECT_EQ(lock.get_sub_capacity(), 0);
}

TEST(NNInt128LockTest, NegativeBaseRejectsAddPastCapacity)
{
  constexpr int128_t kBaseValue = -100;
  NNInt128Lock lock(kBaseValue);

  NNInt128 small_add(kBaseValue, 100);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &small_add,
                                          make_dummy_context())
                  .has_value());

  NNInt128 large_add(kBaseValue, std::numeric_limits<int128_t>::max());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &large_add,
                                          make_dummy_context())
                  .has_value());

  NNInt128 extra_add(kBaseValue, 1);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &extra_add,
                                           make_dummy_context())
                   .has_value());
}

TEST(NNInt128LockTest, StageCommittedChangesRejectsWrappedOutOfBoundsValue)
{
  NNInt128Lock lock(-100);
  lock.delta_bounds_.delta_.store(std::numeric_limits<int128_t>::min() + 100,
                                  std::memory_order_relaxed);

  auto &dao = test_dao();
  auto changes = dao.create_changeset();

  EXPECT_DEATH(
      {
        auto ctx = make_persist_context();
        auto status = lock.stage_committed_changes(ctx, changes);
        (void)status;
      },
      "CHECK FAILED");
}

} // namespace hogd
