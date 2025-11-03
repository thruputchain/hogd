#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel = StorageCostModel(12000, 1, 1, 1, 1);

OwningSpan<const std::byte>
make_owning_span(std::vector<uint8_t> bytes)
{
  auto span = std::as_bytes(std::span(bytes));
  return OwningSpan(span, [ref = std::move(bytes)] {});
}

} // namespace

TEST(BytestringLockTest, BasicUsage)
{
  BytestringLock lock(make_owning_span({0, 1, 2, 3}));

  Bytes match(make_owning_span({0, 1, 2, 3}));
  Bytes nomatch(make_owning_span({4, 5, 6, 7}));

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &match, &kTestCostModel)
                  .has_value());
  EXPECT_TRUE(
      lock.try_atomic_change_lock(&match, &match, &kTestCostModel).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&match, nullptr, &kTestCostModel)
                  .has_value());

  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &nomatch, &kTestCostModel)
                   .has_value());
  EXPECT_FALSE(lock.try_atomic_change_lock(&match, &nomatch, &kTestCostModel)
                   .has_value());
}

} // namespace hogd
