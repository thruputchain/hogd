#include <gtest/gtest.h>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
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

TEST_F(ItemChangeAuthTest, NarrowConflict)
{
  AuthDataItem auth_data_item(base_data_);

  AuthDataItem auth_data_2(base_data_);
  auto pk_1 = make_pk_config(Weight{10});
  auth_data_2.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.narrow(auth_data_2));
  EXPECT_TRUE(auth_data_item.narrow(auth_data_2));

  AuthDataItem auth_data_3(base_data_);
  auth_data_3.set_new_pk(0, nullptr);

  {
    AuthDataItem tmp = auth_data_item;
    EXPECT_FALSE(tmp.narrow(auth_data_3));
  }
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

} // namespace hogd
