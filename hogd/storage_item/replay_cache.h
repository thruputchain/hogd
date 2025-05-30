#pragma once

#include <cstddef>
#include <set>

#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class ReplayCache {
public:
  bool contains(ReplayNonce nonce) const;
  bool can_accept(ReplayNonce nonce) const;

  size_t size() const { return nonces_.size(); }

  auto const& metadata() const {
    return metadata_;
  }

private:
  ReplayCacheMetadata metadata_;
  std::set<ReplayNonce> nonces_;
};

} // namespace hogd
