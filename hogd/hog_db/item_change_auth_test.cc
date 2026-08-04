#include <gtest/gtest.h>

#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/pointer_lock.h"
#include "hogd/storage_cost/storage_cost_model.h"
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

// Helper to create a dummy LockContext for testing
inline LockContext
make_dummy_context(const StorageCostModel &cost_model)
{
  // These are never dereferenced in most lock implementations
  static HogDbDAO *dummy_dao = nullptr;
  static ItemKey dummy_key = make_item_key(make_address(0), 0);
  return LockContext(dummy_dao, 0, dummy_key, &cost_model);
}

} // namespace

class ItemChangeAuthTest : public testing::Test {
protected:
  ItemChangeAuthTest() = default;
  ~ItemChangeAuthTest() override = default;

  void SetUp() override
  {
    base_data_ = AddressAuthData{.public_keys = {
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

  AddressAuthData base_data_;
  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 1000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 1000,
      .contract_deployment_cost_per_byte = 10,
  });
};

/*
TEST_F(ItemChangeAuthTest, MergeConflict)
{
  AuthDataItem auth_data_item(base_data_);

  auto pk_1 = make_pk_config(Weight{10});

  auth_data_item.set_new_pk(0, pk_1);

  AuthDataItem auth_data_2(base_data_);
  auth_data_2.set_new_pk(0, make_pk_config(Weight{10}));

  auto r = auth_data_item.merge(auth_data_2, &cost_model_);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), HogDbError(HogDbCode::kConflict));

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.merge(auth_data_3, &cost_model_).has_value());
}

*/

TEST_F(ItemChangeAuthTest, Narrow)
{
  AuthDataItem auth_data_item(base_data_);

  AuthDataItem auth_data_2(base_data_);
  auto pk_1 = make_pk_config(Weight{10});
  auth_data_2.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.narrow(auth_data_2).value());
  EXPECT_TRUE(auth_data_item.narrow(auth_data_2).value());

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, nullptr);

  EXPECT_TRUE(auth_data_item.narrow(auth_data_3).value());

  ItemLock lock(0);
  ItemChange change = ItemChange(std::move(auth_data_item));

  auto r = lock.try_atomic_change_lock(nullptr, &change,
                                       make_dummy_context(cost_model_));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), cost_model_.get_auth_data_cost(3));

  EXPECT_EQ(lock.commit_change(change, make_dummy_context(cost_model_)),
            cost_model_.get_auth_data_cost(3));

  auto const *inner = lock.get_inner<PointerLock<AuthLock>>();
  ASSERT_TRUE(inner != nullptr);
  auto res = inner->get()->build_result(/*block_height=*/0);

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
  EXPECT_TRUE(narrowed.narrow(auth_data_2).value());

  ItemChange change = ItemChange(std::move(narrowed));

  ItemLock lock(0);
  auto r = lock.try_atomic_change_lock(nullptr, &change,
                                       make_dummy_context(cost_model_));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), cost_model_.get_auth_data_cost(3));

  EXPECT_EQ(lock.commit_change(change, make_dummy_context(cost_model_)),
            cost_model_.get_auth_data_cost(3));

  auto const *inner = lock.get_inner<PointerLock<AuthLock>>();
  ASSERT_TRUE(inner != nullptr);
  auto res = inner->get()->build_result(/*block_height=*/0);

  EXPECT_TRUE(res.public_keys[0]);
  EXPECT_FALSE(res.public_keys[1]);
  EXPECT_TRUE(res.public_keys[3]);
}

/*

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

*/

TEST_F(ItemChangeAuthTest, CommitAndBuild)
{
  AuthDataItem auth_data_item(base_data_);

  auto pk_1 = make_pk_config(Weight{10});
  auth_data_item.set_new_pk(0, pk_1);
  auth_data_item.set_new_pk(1, nullptr);

  ItemLock lock(0);
  ItemChange change = ItemChange(std::move(auth_data_item));
  auto r = lock.try_atomic_change_lock(nullptr, &change,
                                       make_dummy_context(cost_model_));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), cost_model_.get_auth_data_cost(3));

  EXPECT_EQ(lock.commit_change(change, make_dummy_context(cost_model_)),
            cost_model_.get_auth_data_cost(3));

  auto const *inner = lock.get_inner<PointerLock<AuthLock>>();
  ASSERT_TRUE(inner != nullptr);
  auto res = inner->get()->build_result(/*block_height=*/0);

  EXPECT_EQ(res.public_keys[0], pk_1);
  EXPECT_EQ(res.public_keys[1], nullptr);
}

TEST_F(ItemChangeAuthTest, DeleteKey)
{
  AuthDataItem auth_data_item(base_data_);

  auth_data_item.set_new_pk(3, nullptr);

  ItemLock lock(0);
  ItemChange change = ItemChange(std::move(auth_data_item));
  auto r = lock.try_atomic_change_lock(nullptr, &change,
                                       make_dummy_context(cost_model_));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), cost_model_.get_auth_data_cost(3));

  EXPECT_EQ(lock.commit_change(change, make_dummy_context(cost_model_)),
            cost_model_.get_auth_data_cost(2));

  auto const *inner = lock.get_inner<PointerLock<AuthLock>>();
  ASSERT_TRUE(inner != nullptr);
  auto res = inner->get()->build_result(/*block_height=*/0);

  EXPECT_EQ(res.public_keys[3], nullptr);
}

} // namespace hogd
