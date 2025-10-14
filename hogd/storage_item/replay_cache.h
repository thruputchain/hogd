#pragma once

#include <cstddef>
#include <utility>

#include "hogd/base/types.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ReplayCache {
public:
  ReplayCache(BlockHeight expired_height) : expired_height_(expired_height) {}

  ReplayCache(ReplayCacheData data, BlockHeight expired_height)
    : data_(std::move(data)), expired_height_(expired_height)
  {
    filter_expired();
  }

  bool contains(Uniquifier uniquifier) const;
  bool can_accept(Uniquifier uniquifier) const;

  void add(Uniquifier uniquifier);
  void commit_add(Uniquifier uniquifier);
  void remove(Uniquifier uniquifier);

  ReplayCacheData const &data() const { return data_; }

  size_t committed_size() const { return committed_size_; }

  BlockHeight expired_height() const { return expired_height_; }

private:
  ReplayCacheData data_;
  BlockHeight expired_height_;

  size_t committed_size_;

  void filter_expired();
};

} // namespace hogd
