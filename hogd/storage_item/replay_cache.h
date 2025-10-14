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
  ReplayCache(ReplayCacheData data, BlockHeight expired_height)
    : data_(std::move(data)), expired_height_(expired_height),
    committed_size_(data_.uniquifiers.size())
  {
    filter_expired();
  }

  bool contains(Uniquifier uniquifier) const;
  bool can_accept(Uniquifier uniquifier) const;

  void add(Uniquifier uniquifier);
  void commit_add(Uniquifier uniquifier);
  void remove(Uniquifier uniquifier);

  size_t size() const { return data_.uniquifiers.size(); }

  ReplayCacheData const& data() const {
    return data_;
  }

  size_t committed_size() const {
    return committed_size_;
  }

private:
  ReplayCacheData data_;
  BlockHeight expired_height_;

  size_t committed_size_;

  void filter_expired();
};

} // namespace hogd
