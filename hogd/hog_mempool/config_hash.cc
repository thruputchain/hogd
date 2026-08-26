#include "hogd/hog_mempool/config_hash.h"

#include "hogd/crypto/crypto.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

ConfigHashPreimage
make_config_hash_preimage(StorageCostModel const &storage_cost_model)
{
  return ConfigHashPreimage{
      .storage_cost_model = storage_cost_model.to_xdr(),
  };
}

ConfigHash
compute_config_hash(ConfigHashPreimage const &preimage)
{
  return sha256(preimage);
}

ConfigHash
compute_config_hash(StorageCostModelXdr const &storage_cost_model)
{
  return compute_config_hash(
      ConfigHashPreimage{.storage_cost_model = storage_cost_model});
}

} // namespace hogd
