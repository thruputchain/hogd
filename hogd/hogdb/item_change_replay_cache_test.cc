#include <gtest/gtest.h>
#include <stdexcept>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "storage_cost.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

class ItemChangeReplayCacheTest : public testing::Test {
protected:
  ItemChangeReplayCacheTest() = default;
  ~ItemChangeReplayCacheTest() override = default;

  void SetUp() override
  {
    base_data_ = ReplayCacheData{.uniquifiers = {
                                     {10, 100},
                                 }};
  }

  ReplayCacheData base_data_;
  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = to_xdr(int128_t{1000}),
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = to_xdr(int128_t{1000}),
      .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
  });
};

TEST_F(ItemChangeReplayCacheTest, DisallowDuplicateConsume)
{
  ReplayCacheIt cache1(base_data_);
  // conflicts with a nonce in base_data_
  auto r = cache1.try_consume_uniquifier(10, {11, 100});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));

  // doesn't conflict, added as an addition
  r = cache1.try_consume_uniquifier(10, {11, 101});
  ASSERT_TRUE(r.has_value());

  // conflicts with the previous addition
  r = cache1.try_consume_uniquifier(10, {11, 101});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ItemChangeReplayCacheTest, DisallowConsumeInThePast)
{
  ReplayCacheIt cache1(base_data_);
  auto r = cache1.try_consume_uniquifier(10, {10, 100});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ItemChangeReplayCacheTest, MergeConflict)
{
  ReplayCacheIt cache1(base_data_);
  ReplayCacheIt cache2(ReplayCacheData{.uniquifiers = {{11, 100}}});

  // this shouldn't happen in practice since merging replay caches
  // build on the same item snapshot, but we conservatively fail
  // the merge if they don't have identical base caches.
  auto r = cache1.merge(cache2, &cost_model_);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));

  cache1.try_consume_uniquifier(11, {11, 100});
  ReplayCacheIt cache3(base_data_);

  // cache1 contains nonce 100, cache3 only has the base 99 nonce
  // so merge is okay.
  EXPECT_TRUE(cache1.merge(cache3, &cost_model_).has_value());

  // now cache3 shouldn't merge since they both
  cache3.try_consume_uniquifier(11, {12, 100});

  r = cache1.merge(cache3, &cost_model_);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));
}

} // namespace hogd
