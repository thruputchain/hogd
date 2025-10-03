#include <gtest/gtest.h>
#include <stdexcept>
#include <utility>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "storage_cost.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

namespace {
PublicKeyConfigOpt
make_pk_config(Weight w)
{
  auto [pk, _] = ed25519_keygen();
  PublicKey pk_wrapper(CryptographicScheme::Ed25519);
  pk_wrapper.ed25519() = pk;
  return x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = pk_wrapper, .max_weight = w});
}
} // namespace

class ItemChangeAuthTest : public testing::Test {
protected:
  ItemChangeAuthTest() = default;
  ~ItemChangeAuthTest() override = default;

  void SetUp() override
  {
    base_data_ = AccountAuthData{.public_keys = {
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     make_pk_config(Weight{1}),
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     make_pk_config(Weight{2}),
                                     nullptr,
                                     make_pk_config(Weight{3}),
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                 }};
  }

  AccountAuthData base_data_;
  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = to_xdr(int128_t{1000}),
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = to_xdr(int128_t{1000}),
      .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
  });
};

TEST_F(ItemChangeAuthTest, MergeConflict)
{
  AuthDataItem auth_data_item(base_data_);

  auto pk_1 = make_pk_config(Weight{10});

  auth_data_item.set_new_pk(0, pk_1);

  AuthDataItem auth_data_2(base_data_);
  auth_data_2.set_new_pk(0, make_pk_config(Weight{10}));

  auto r = auth_data_item.merge(auth_data_2, &cost_model_);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDBError(HogDBCode::kConflict));

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.merge(auth_data_3, &cost_model_).has_value());
}

TEST_F(ItemChangeAuthTest, Narrow)
{
  AuthDataItem auth_data_item(base_data_);

  AuthDataItem auth_data_2(base_data_);
  auto pk_1 = make_pk_config(Weight{10});
  auth_data_2.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.narrow(auth_data_2));
  EXPECT_TRUE(auth_data_item.narrow(auth_data_2));

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, nullptr);

  EXPECT_TRUE(auth_data_item.narrow(auth_data_3));

  auto res = auth_data_item.build_result();
  EXPECT_FALSE(res.public_keys[0]);
  EXPECT_FALSE(res.public_keys[1]);
  EXPECT_FALSE(res.public_keys[2]);
}

TEST_F(ItemChangeAuthTest, NarrowUncommitted)
{
  AuthDataItem auth_data_item(base_data_);
  auth_data_item.set_new_pk(0, make_pk_config(Weight{10}));

  // This is a write over a key that already exists in the base data,
  // and a write over a key that doesn't exist in the base data.
  AuthDataItem auth_data_2(base_data_);
  auth_data_2.set_new_pk(1, make_pk_config(Weight{10}));
  auth_data_2.set_new_pk(3, make_pk_config(Weight{10}));

  AuthDataItem narrowed = auth_data_item;
  EXPECT_TRUE(narrowed.narrow(auth_data_2));

  narrowed.commit_change(auth_data_item);

  auto res = narrowed.build_result();
  EXPECT_TRUE(res.public_keys[0]);
  EXPECT_FALSE(res.public_keys[1]);
  EXPECT_TRUE(res.public_keys[3]);
}

TEST_F(ItemChangeAuthTest, NarrowExclusiveConflict)
{
  AuthDataItem auth_data_item(base_data_);
  auth_data_item.set_new_pk(0, make_pk_config(Weight{10}));
  AuthDataItem auth_data_2(base_data_);
  auth_data_2.set_new_pk(0, nullptr);
  EXPECT_TRUE(auth_data_item.narrow(auth_data_2));

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, make_pk_config(Weight{10}));

  EXPECT_FALSE(auth_data_item.merge(auth_data_3, &cost_model_).has_value());
}

TEST_F(ItemChangeAuthTest, MergeAndUndo)
{
  AuthDataItem auth_data_item(base_data_);

  auto pk_1 = make_pk_config(Weight{10});
  AuthDataItem auth_data_2(base_data_);
  auth_data_2.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.merge(auth_data_2, &cost_model_).has_value());

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, nullptr);

  EXPECT_FALSE(auth_data_item.merge(auth_data_3, &cost_model_).has_value());

  auth_data_item.undo_merge(auth_data_2);

  EXPECT_TRUE(auth_data_item.merge(auth_data_3, &cost_model_).has_value());
}

TEST_F(ItemChangeAuthTest, CommitAndBuild)
{
  AuthDataItem auth_data_item(base_data_);

  auto pk_1 = make_pk_config(Weight{10});
  auth_data_item.set_new_pk(0, pk_1);
  auth_data_item.set_new_pk(1, nullptr);

  auth_data_item.commit_change(auth_data_item);

  auto res = auth_data_item.build_result();
  EXPECT_EQ(res.public_keys[0], pk_1);
  EXPECT_EQ(res.public_keys[1], nullptr);
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// These tests check that the cost result from change.merge()
// matches that returned by change.marginal_storage_cost().
// This is required to guarantee that block proposal matches block validation,
// until we enable retrocausal nnints.
TEST_F(ItemChangeAuthTest, MergeCosts)
{
  AuthDataItem base(base_data_);
  const auto pk_0 = make_pk_config(Weight{10});
  const auto pk_1 = make_pk_config(Weight{10});
  base.set_new_pk(0, pk_0);
  base.set_new_pk(1, pk_1);
  ItemChange compare = ItemChange(std::move(base));
  // base_data_ has 3 public keys
  compare.set_snapshot_cost(cost_model_.get_auth_data_cost(3));

  int128_t expected_cost = compare.marginal_storage_cost(&cost_model_).value();

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    model::RunnableActionSet actions(work_queue);

    ItemChange merged = compare;

    // the actual query action
    actions.add_action(
        [](model::RunnableActionSet &set, auto self, ItemChange &merged,
           const auto &pk0, const auto &pk1,
           int128_t &expected_cost) -> model::Async {
          co_await set.bg();

          AuthDataItem base(self->base_data_);
          base.set_new_pk(0, pk0);
          base.set_new_pk(1, pk1);
          auto change1 = ItemChange(std::move(base));

          auto r = merged.merge(change1, &self->cost_model_);

          if (!r.has_value()) {
            // this coroutine just exits
            co_return;
          }

          if (r.value() != expected_cost) {
            throw std::runtime_error("Merge cost mismatch");
          }

          co_return;
        },
        this, merged, pk_0, pk_1, expected_cost);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self,
           ItemChange &merged) -> model::Async {
          co_await set.bg();

          AuthDataItem base(self->base_data_);
          base.set_new_pk(0, nullptr);
          base.set_new_pk(2, make_pk_config(Weight{10}));
          auto change1 = ItemChange(std::move(base));

          auto r = merged.merge(change1, &self->cost_model_);

          if (!r.has_value()) {
            // this coroutine just exits
            co_return;
          }

          co_await set.bg();
          auto choice = set.choice(2);
          if (choice == 0) {
            merged.commit_change(change1);
          }
          else {
            merged.undo_merge(change1);
          }
        },
        this, merged);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self, ItemChange &merged,
           const auto &pk0) -> model::Async {
          co_await set.bg();

          AuthDataItem base(self->base_data_);
          base.set_new_pk(0, pk0);
          auto change1 = ItemChange(std::move(base));

          auto r = merged.merge(change1, &self->cost_model_);

          if (!r.has_value()) {
            // this coroutine just exits
            co_return;
          }

          co_await set.bg();
          auto choice = set.choice(2);
          if (choice == 0) {
            merged.commit_change(change1);
          }
          else {
            merged.undo_merge(change1);
          }
        },
        this, merged, pk_0);

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    work_queue.advance_cursor();
  }
}

TEST_F(ItemChangeAuthTest, MergeCosts2)
{
  AuthDataItem base(base_data_);
  const auto pk_0 = make_pk_config(Weight{10});
  const auto pk_1 = make_pk_config(Weight{10});
  base.set_new_pk(0, nullptr);
  base.set_new_pk(1, pk_1);
  base.set_new_pk(3, nullptr);
  ItemChange compare = ItemChange(std::move(base));
  // base_data_ has 3 public keys
  compare.set_snapshot_cost(cost_model_.get_auth_data_cost(3));

  int128_t expected_cost = compare.marginal_storage_cost(&cost_model_).value();

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    model::RunnableActionSet actions(work_queue);

    ItemChange merged = compare;

    // the actual query action
    actions.add_action(
        [](model::RunnableActionSet &set, auto self, ItemChange &merged,
           const auto &pk1, int128_t &expected_cost) -> model::Async {
          co_await set.bg();

          AuthDataItem base(self->base_data_);
          base.set_new_pk(0, nullptr);
          base.set_new_pk(1, pk1);
          base.set_new_pk(3, nullptr);
          auto change1 = ItemChange(std::move(base));

          auto r = merged.merge(change1, &self->cost_model_);

          if (!r.has_value()) {
            // this coroutine just exits
            co_return;
          }

          if (r.value() != expected_cost) {
            throw std::runtime_error("Merge cost mismatch");
          }

          merged.commit_change(change1);

          co_return;
        },
        this, merged, pk_1, expected_cost);

    actions.add_action(
        [](model::RunnableActionSet &set, auto self,
           ItemChange &merged) -> model::Async {
          co_await set.bg();

          AuthDataItem base(self->base_data_);
          base.set_new_pk(0, nullptr);
          base.set_new_pk(2, make_pk_config(Weight{10}));
          auto change1 = ItemChange(std::move(base));

          auto r = merged.merge(change1, &self->cost_model_);

          if (!r.has_value()) {
            throw std::runtime_error("Merge cost mismatch");
          }

          co_await set.bg();
          auto choice = set.choice(2);
          if (choice == 0) {
            merged.commit_change(change1);
            co_return;
          }
          merged.undo_merge(change1);

          AuthDataItem base2(self->base_data_);
          base2.set_new_pk(3, nullptr);

          auto change2 = ItemChange(std::move(base2));

          change2.narrow(change1);

          r = merged.merge(change2, &self->cost_model_);

          if (!r.has_value()) {
            throw std::runtime_error("Merge cost mismatch");
          }

          co_await set.bg();
          choice = set.choice(2);

          if (choice == 0) {
            merged.commit_change(change2);
            co_return;
          }

          merged.undo_merge(change2);
        },
        this, merged);

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    ASSERT_TRUE(merged.to_auth_data().has_value());
    auto auth_data = merged.to_auth_data().value().build_result();

    EXPECT_EQ(auth_data.public_keys[0], nullptr);
    EXPECT_EQ(auth_data.public_keys[1], pk_1);
    EXPECT_EQ(auth_data.public_keys[3], nullptr);

    work_queue.advance_cursor();
  }
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
