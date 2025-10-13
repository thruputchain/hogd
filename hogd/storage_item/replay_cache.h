#pragma once

#include <cstddef>
#include <utility>

#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ReplayCache {
public:
  ReplayCache(ReplayCacheData data, BlockHeight expired_height)
    : data_(std::move(data)), expired_height_(expired_height)
  {
    filter_expired();
  }

  bool contains(Uniquifier uniquifier) const;
  bool can_accept(Uniquifier uniquifier) const;

  size_t size() const { return data_.uniquifiers.size(); }

  auto const &uniquifiers() const { return data_.uniquifiers; }

private:
  ReplayCacheData data_;
  BlockHeight expired_height_;

  void filter_expired();
};

} // namespace hogd
