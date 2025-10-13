#include "hogd/storage_item/replay_cache.h"

#include "storage_item.hh"
#include "transaction.hh"
#include <algorithm>
#include <cstddef>
#include <utility>

namespace hogd {

bool
ReplayCache::contains(Uniquifier uniquifier) const
{
  return std::ranges::find(data_.uniquifiers, uniquifier) != data_.uniquifiers.end();
}

bool
ReplayCache::can_accept(Uniquifier uniquifier) const
{
  if (uniquifier.block_expiry_height <= expired_height_) {
    return false;
  }

  return !contains(uniquifier);
}

void
ReplayCache::filter_expired()
{
  ReplayCacheData new_data;
  new_data.uniquifiers.reserve(data_.uniquifiers.size());
  for (Uniquifier const &uniquifier : data_.uniquifiers) {
    if (uniquifier.block_expiry_height > expired_height_) {
      new_data.uniquifiers.push_back(uniquifier);
    }
  }
  data_ = std::move(new_data);
}

} // namespace hogd
