
// clang-format off
%#include "types.hh"

namespace hogd {
  // clang-format on

  typedef opaque ContractBlob<>;

  struct Contract {
    ContractBlob contract_data;
    // metadata for a future PR
  };

  struct ContractDeploymentHashPreimage {
    ContractAddress deployer_address;
    SHA256Hash nonce;
    SHA256Hash contract_hash;
  };

} // namespace hogd
