#include "hogd/storage_item/replay_cache.h"

#include "types.hh"

namespace hogd {

bool
ReplayCache::contains(ReplayNonce nonce) const
{
  return nonces_.contains(nonce);
}

bool
ReplayCache::can_accept(ReplayNonce nonce) const
{
  if (nonce <= metadata_.minimum_threshold) {
    return false;
  }

  return !contains(nonce);
}

} // namespace hogd
