#pragma once

#include <cstddef>
#include <set>
#include <utility>

#include "hogd/base/types.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class ReplayCache {
public:
  ReplayCache(ReplayCacheMetadata metadata, std::set<ReplayNonce> nonces)
    : metadata_(std::move(metadata)), nonces_(std::move(nonces))
  {}

  bool contains(ReplayNonce nonce) const;
  bool can_accept(ReplayNonce nonce) const;

  size_t size() const { return nonces_.size(); }

  auto const &metadata() const { return metadata_; }

  auto const &nonces() const { return nonces_; }

  size_t count_remaining_after_clear(ReplayNonce threshold) const;

private:
  ReplayCacheMetadata metadata_;
  std::set<ReplayNonce> nonces_;
};

} // namespace hogd
