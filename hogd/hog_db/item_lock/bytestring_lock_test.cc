#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/bytestring_lock.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);

OwningSpan<const std::byte>
make_owning_span(std::vector<uint8_t> bytes)
{
  auto span = std::as_bytes(std::span(bytes));
  return OwningSpan(span, [ref = std::move(bytes)] {});
}

// Helper to create a dummy LockContext for testing
inline LockContext
make_dummy_context()
{
  // These are never dereferenced in most lock implementations
  static HogDbDAO *dummy_dao = nullptr;
  static ItemKey dummy_key = make_item_key(make_address(0), 0);
  return LockContext(dummy_dao, 0, dummy_key, &kTestCostModel);
}

} // namespace

TEST(BytestringLockTest, BasicUsage)
{
  BytestringLock lock(make_owning_span({0, 1, 2, 3}));

  Bytes match(make_owning_span({0, 1, 2, 3}));
  Bytes nomatch(make_owning_span({4, 5, 6, 7}));

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &match, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&match, &match, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&match, nullptr, make_dummy_context())
                  .has_value());

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &nomatch, make_dummy_context())
          .has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&match, &nomatch, make_dummy_context())
          .has_value());
}

TEST(BytestringLockTest, CommitCost)
{
  BytestringLock lock(make_owning_span({0, 1, 2, 3}));

  Bytes item1(make_owning_span({0, 1, 2, 3}));

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &item1, make_dummy_context())
                  .has_value());
  EXPECT_EQ(lock.commit_change(true, 0, item1, make_dummy_context()),
            kTestCostModel.get_bytestring_cost(4));
  EXPECT_EQ(lock.commit_change(false, 0, item1, make_dummy_context()), 0);
}
} // namespace hogd
