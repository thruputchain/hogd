#pragma once

#include <utility>

#include "genesis.hh"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Address of the initial token claim precompile. The replay cache is disabled
// for it: it manages replay protection itself, via the exclusive deletes of its
// stored claim entries, so the executor consumes no uniquifier for a
// transaction rooted at this address, and HogDB rejects any replay-cache delta
// keyed here as invalid executor behavior. (A replay cache here would also be a
// DoS vector, since anyone could fill it with failing transactions, which the
// contract could not prevent.) This mirrors GenesisConfigurationLoader's
// derivation of the same precompile address.
constexpr ContractAddress kInitialTokenClaimPrecompileAddress = [] {
  ContractAddress addr{};
  uint32_t underlying =
      std::to_underlying(GenesisContractAddresses::kAddressInitialTokenClaim);
  addr[28] = (underlying >> 24) & 0xFF;
  addr[29] = (underlying >> 16) & 0xFF;
  addr[30] = (underlying >> 8) & 0xFF;
  addr[31] = underlying & 0xFF;
  return addr;
}();

class ReplayCache {
public:
  ReplayCache(BlockHeight expired_height)
    : expired_height_(expired_height), committed_size_(0)
  {}

  ReplayCache(std::vector<Uniquifier> uniquifiers, BlockHeight expired_height)
    : uniquifiers_(std::move(uniquifiers)), expired_height_(expired_height)
  {
    filter_expired();
    committed_size_ = uniquifiers_.size();
  }

  bool contains(Uniquifier uniquifier) const;
  bool can_accept(Uniquifier uniquifier) const;

  void add(Uniquifier uniquifier);
  void commit_add(Uniquifier uniquifier);
  void remove(Uniquifier uniquifier);

  std::vector<Uniquifier> const &uniquifiers() const { return uniquifiers_; }

  size_t committed_size() const { return committed_size_; }

  BlockHeight expired_height() const { return expired_height_; }

private:
  std::vector<Uniquifier> uniquifiers_;
  BlockHeight expired_height_;

  size_t committed_size_;

  void filter_expired();
};

} // namespace hogd
