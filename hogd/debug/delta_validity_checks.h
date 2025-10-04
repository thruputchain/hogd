#pragma once

#include <unordered_set>

#include "account.hh"
#include "hogd/base/types.h"
#include "storage_item.hh"

namespace hogd {

inline bool
is_valid_delta(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeAuthDataChange:
  {
    std::unordered_set<uint32_t> key_ids;
    for (const auto &signer :
         delta.auth_data_change().auth_data_change_signers) {
      key_ids.insert(signer.public_key_id);
      if (signer.public_key_id >= MAX_SIGNERS) {
        return false;
      }
    }
    return key_ids.size() ==
           delta.auth_data_change().auth_data_change_signers.size();
  }
  default:
    return true;
  }
}

} // namespace hogd
