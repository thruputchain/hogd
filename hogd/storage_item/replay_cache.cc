#include "hogd/storage_item/replay_cache.h"

#include "types.hh"
#include <cstddef>
#include <iterator>

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

size_t
ReplayCache::count_remaining_after_clear(ReplayNonce threshold) const
{
  return std::distance(nonces_.upper_bound(threshold), nonces_.end());
}

} // namespace hogd
