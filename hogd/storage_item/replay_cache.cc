#include "hogd/storage_item/replay_cache.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

#include "transaction.hh"

namespace hogd {

bool
ReplayCache::contains(Uniquifier uniquifier) const
{
  return std::ranges::find(uniquifiers_, uniquifier) != uniquifiers_.end();
}

bool
ReplayCache::can_accept(Uniquifier uniquifier) const
{
  if (uniquifier.block_expiry_height < expired_height_) {
    return false;
  }

  return !contains(uniquifier);
}

void
ReplayCache::add(Uniquifier uniquifier)
{
  assert(can_accept(uniquifier));
  uniquifiers_.push_back(uniquifier);
}

void
ReplayCache::commit_add(Uniquifier uniquifier)
{
  assert(contains(uniquifier));
  committed_size_++;
}

void
ReplayCache::remove(Uniquifier uniquifier)
{
  assert(contains(uniquifier));
  uniquifiers_.erase(std::ranges::find(uniquifiers_, uniquifier));
}

void
ReplayCache::filter_expired()
{
  std::vector<Uniquifier> new_data;
  new_data.reserve(uniquifiers_.size());
  for (Uniquifier const &uniquifier : uniquifiers_) {
    if (uniquifier.block_expiry_height >= expired_height_) {
      new_data.push_back(uniquifier);
    }
  }
  uniquifiers_ = std::move(new_data);
}

} // namespace hogd
