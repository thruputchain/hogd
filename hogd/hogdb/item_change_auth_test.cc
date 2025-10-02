#include <gtest/gtest.h>

#include "account.hh"
#include "hogd/hogdb/item_change.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"

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
  auto base_data = AccountAuthData{.public_keys = {
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
  AuthDataItem auth_data_item(base_data);

  auto pk_1 = make_pk_config(Weight{10});

  auth_data_item.set_new_pk(0, pk_1);

  AuthDataItem auth_data_2(base_data);
  auth_data_2.set_new_pk(0, make_pk_config(Weight{10}));

  EXPECT_EQ(auth_data_item.merge(auth_data_2, &cost_model_).error(),
            HogDBError(HogDBCode::kConflict));

  AuthDataItem auth_data_3(base_data);
  auth_data_3.set_new_pk(0, pk_1);

  EXPECT_TRUE(auth_data_item.merge(auth_data_3, &cost_model_).has_value());
}

} // namespace hogd
