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
    height_ = 9;
    base_data_ = ReplayCacheData{.uniquifiers = {
                                     {height_ + 1, 100},
                                 }};
    entry_cost_ = cost_model_.get_replay_cache_cost(1);
  }

  BlockHeight height_;
  ReplayCacheData base_data_;
  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = to_xdr(int128_t{1000}),
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = to_xdr(int128_t{1000}),
      .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
      .replay_cache_cost_per_entry = to_xdr(int128_t{7}),
  });
  int128_t entry_cost_;
};

TEST_F(ItemChangeReplayCacheTest, DisallowDuplicateConsume)
{
  ReplayCacheIt cache1(9, base_data_);
  // conflicts with a nonce in base_data_
  auto r = cache1.try_consume_uniquifier({11, 100});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));

  // doesn't conflict, added as an addition
  r = cache1.try_consume_uniquifier({11, 101});
  ASSERT_TRUE(r.has_value());

  // conflicts with the previous addition
  r = cache1.try_consume_uniquifier({11, 101});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ItemChangeReplayCacheTest, DisallowConsumeInThePast)
{
  ReplayCacheIt cache1(9, base_data_);
  auto r = cache1.try_consume_uniquifier({10, 100});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));
}

TEST_F(ItemChangeReplayCacheTest, MergeConflict)
{
  ReplayCacheIt cache1(height_, base_data_);
  ReplayCacheIt cache2(height_,
                       ReplayCacheData{.uniquifiers = {{height_ + 1, 101}}});

  // this shouldn't happen in practice since merging replay caches
  // build on the same item snapshot, but we conservatively fail
  // the merge if they don't have identical base caches.
  auto before = cache1;
  auto rm = cache1.merge(cache2, &cost_model_);
  ASSERT_FALSE(rm.has_value());
  EXPECT_EQ(rm.error(), HogDBError(HogDBCode::kConflict));
  EXPECT_EQ(before.additions_, cache1.additions_); // no change to cache1

  // consume 101 in cache1, merge a new cache -> ok
  auto r = cache1.try_consume_uniquifier({height_ + 1, 101});
  ASSERT_TRUE(r.has_value());
  ReplayCacheIt cache3(height_, base_data_);
  EXPECT_TRUE(cache1.merge(cache3, &cost_model_).has_value());

  // add 101 to cache3, merge -> fail, both contain 101 as additions
  r = cache3.try_consume_uniquifier({height_ + 2, 101});
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(cache1.merge(cache3, &cost_model_).has_value());
}

TEST_F(ItemChangeReplayCacheTest, UndoMerge)
{
  ReplayCacheIt c1(height_, base_data_);
  ReplayCacheIt c2(height_, base_data_);

  ASSERT_TRUE(c2.try_consume_uniquifier({height_ + 1, 101}));

  ASSERT_TRUE(c1.merge(c2, &cost_model_));
  // c2 merge put 101 in cache, so c1 fails to add 101
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 101}));

  // unmerging c2 means 101 shouldn't be in the cache anymore
  c1.undo_merge(c2);
  ASSERT_TRUE(c1.try_consume_uniquifier({height_ + 1, 101}));

  // a more complicated case where three nonces are merged,
  // one is reverted, but ensure not all are reverted
  ReplayCacheIt c3(height_, base_data_);
  ASSERT_TRUE(c3.try_consume_uniquifier({height_ + 1, 103}));
  ReplayCacheIt c4(height_, base_data_);
  ASSERT_TRUE(c4.try_consume_uniquifier({height_ + 1, 104}));

  ASSERT_TRUE(c1.merge(c3, &cost_model_));
  ASSERT_TRUE(c1.merge(c4, &cost_model_));

  // fails - 100, 101 in cache due to c1
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 100}));
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 101}));
  // fails - 103 in cache due to c3
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 103}));
  // fails - 104 in cache due to c4
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 104}));

  c1.undo_merge(c3);
  // fails - 100, 101 in cache due to c1
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 100}));
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 101}));
  // success - 103 not in cache due to c3 undo
  ASSERT_TRUE(c1.try_consume_uniquifier({height_ + 1, 103}));
  // fails - 104 in cache due to c3
  ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 104}));
}

TEST_F(ItemChangeReplayCacheTest, Expiry)
{
  {
    ReplayCacheIt c1(height_, base_data_);
    ASSERT_FALSE(c1.try_consume_uniquifier({height_, 101}));
    ASSERT_FALSE(c1.try_consume_uniquifier({height_ + 1, 100}));
    ASSERT_TRUE(c1.try_consume_uniquifier({height_ + 1, 101}));
  }

  height_++;

  {
    ReplayCacheIt c1(height_, base_data_);
    ASSERT_FALSE(c1.try_consume_uniquifier({height_, 101}));
    ASSERT_FALSE(c1.try_consume_uniquifier({height_, 100}));
    ASSERT_TRUE(c1.try_consume_uniquifier({height_ + 1, 100}));
    ASSERT_TRUE(c1.try_consume_uniquifier({height_ + 1, 101}));

    ReplayCacheData out = c1.build_result();
    ASSERT_EQ(2, out.uniquifiers.size());
    EXPECT_EQ(100, out.uniquifiers[0].nonce);
    EXPECT_EQ(101, out.uniquifiers[1].nonce);

    ReplayCacheIt remade(height_, out);
    EXPECT_FALSE(remade.try_consume_uniquifier({height_ + 1, 100}));
    EXPECT_FALSE(remade.try_consume_uniquifier({height_ + 1, 101}));
    EXPECT_TRUE(remade.try_consume_uniquifier({height_ + 1, 102}));
  }
}

TEST_F(ItemChangeReplayCacheTest, Build)
{
  ReplayCacheIt c(height_, base_data_);

  ASSERT_TRUE(c.try_consume_uniquifier({height_ + 1, 101}));
  ASSERT_TRUE(c.try_consume_uniquifier({height_ + 1, 102}));
  ASSERT_TRUE(c.try_consume_uniquifier({height_ + 1, 104}));

  ReplayCacheData out = c.build_result();
  ASSERT_EQ(4, out.uniquifiers.size());
  EXPECT_EQ(100, out.uniquifiers[0].nonce);
  EXPECT_EQ(101, out.uniquifiers[1].nonce);
  EXPECT_EQ(102, out.uniquifiers[2].nonce);
  EXPECT_EQ(104, out.uniquifiers[3].nonce);

  ReplayCacheIt remade(height_, out);
  EXPECT_FALSE(remade.try_consume_uniquifier({height_ + 1, 101}));
  EXPECT_TRUE(remade.try_consume_uniquifier({height_ + 1, 103}));
}

TEST_F(ItemChangeReplayCacheTest, MergeCost)
{
  auto make_change = [this](std::vector<ReplayNonce> &&nonces,
                            std::optional<BlockHeight> height =
                                std::nullopt) -> ItemChange {
    ReplayCacheIt ci{height_, base_data_};
    for (auto nonce : nonces)
      ci.try_consume_uniquifier({height.value_or(height_) + 1, nonce});
    ItemChange c{std::move(ci)};
    // base_data_ contains one existing entry in simulated storage,
    // so simulate its snapshot cost.
    c.set_snapshot_cost(entry_cost_);
    return c;
  };

  // marginal cost must match cost of merge into empty item change
  auto check_marginal_cost_ok = [this, &make_change](ItemChange &c) {
    auto merge_result = make_change({}).merge(c, &cost_model_);
    ASSERT_TRUE(merge_result);
    EXPECT_EQ(*merge_result, c.marginal_storage_cost(&cost_model_).value());
  };

  auto c1 = make_change({});

  { // merging changes with same single base entry adds no cost
    auto r = c1.merge(make_change({}), &cost_model_);
    ASSERT_TRUE(r);
    EXPECT_EQ(0, *r);
    check_marginal_cost_ok(c1);
  }

  { // merging non-conflicting entry results adds cost
    auto r = c1.merge(make_change({101}), &cost_model_);
    ASSERT_TRUE(r);
    EXPECT_EQ(entry_cost_, *r);
    check_marginal_cost_ok(c1);
  }

  { // merging one more non-conflicting entry in requires
    // one more entry worth of cost to be locked
    auto r = c1.merge(make_change({102}), &cost_model_);
    ASSERT_TRUE(r);
    EXPECT_EQ(entry_cost_, *r);
    check_marginal_cost_ok(c1);
  }

  // checks that merges and costs work with uniquifier expiry are in HogDBTest
  // since they are best tested with storage.
}

} // namespace hogd
