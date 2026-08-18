#pragma once

#include <cstdint>

#include "hogd/crypto/crypto.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

// Address of the [index]th synthetic transfer recipient. Nothing is deployed
// at these addresses and no other part of an experiment derives an address
// this way, so the first erc20 transfer to one inserts a balance item that
// would not otherwise exist. Sending to a spread of them is how an experiment
// grows the working set without paying a genesis deployment per address.
inline ContractAddress
make_virtual_address(uint64_t index)
{
  // Domain separator, so these can never collide with a nonce-derived address.
  constexpr uint64_t kVirtualAddressTag = 0x5641'4444'5230'0001;
  return sha256(x23::serialize(kVirtualAddressTag, index));
}

} // namespace hogd
