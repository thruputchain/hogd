#include <gtest/gtest.h>

#include <array>

#include "header.hh"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"

namespace hogd {

TEST(ConfigHashTest, CommitsToXdrPreimage)
{
  const StorageCostModel model = StorageCostModel::default_cost_model();
  const ConfigHash expected{
      0x5a, 0x4a, 0x14, 0xaf, 0xcc, 0x2f, 0xb0, 0xb7, 0xfb, 0x83, 0xc7,
      0xb4, 0xfc, 0xa2, 0x82, 0x3c, 0x06, 0x8b, 0x92, 0xa2, 0xce, 0x70,
      0x5f, 0x91, 0x56, 0x8a, 0xa8, 0x63, 0xe7, 0x1f, 0x1c, 0x21,
  };
  EXPECT_EQ(compute_config_hash(make_config_hash_preimage(model)), expected);

  const ConfigHash original =
      compute_config_hash(make_config_hash_preimage(model));
  StorageCostModelXdr xdr = model.to_xdr();
  std::array<StorageCostModelXdr, 8> variants;
  variants.fill(xdr);
  variants[0].base_data_item_cost++;
  variants[1].bytestring_cost_per_byte++;
  variants[2].base_contract_deployment_cost++;
  variants[3].contract_deployment_cost_per_byte++;
  variants[4].auth_per_key_cost++;
  variants[5].rcq_cost_per_entry++;
  variants[6].replay_cache_cost_per_entry++;
  variants[7].balance_cost++;
  for (auto const &variant : variants) {
    EXPECT_NE(compute_config_hash(
                  make_config_hash_preimage(StorageCostModel(variant))),
              original);
  }
}

} // namespace hogd
