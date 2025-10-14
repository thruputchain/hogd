#include "hogd/storage_item/replay_cache.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

#include "storage_item.hh"
#include "transaction.hh"

namespace hogd {

bool
ReplayCache::contains(Uniquifier uniquifier) const
{
  return std::ranges::find(data_.uniquifiers, uniquifier) !=
         data_.uniquifiers.end();
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
  data_.uniquifiers.push_back(uniquifier);
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
  data_.uniquifiers.erase(std::ranges::find(data_.uniquifiers, uniquifier));
}

void
ReplayCache::filter_expired()
{
  ReplayCacheData new_data;
  new_data.uniquifiers.reserve(data_.uniquifiers.size());
  for (Uniquifier const &uniquifier : data_.uniquifiers) {
    if (uniquifier.block_expiry_height >= expired_height_) {
      new_data.uniquifiers.push_back(uniquifier);
    }
  }
  data_ = std::move(new_data);
  committed_size_ = data_.uniquifiers.size();
}

} // namespace hogd
