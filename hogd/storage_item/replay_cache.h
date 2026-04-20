#pragma once

#include <utility>

#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ReplayCache {
public:
  ReplayCache(BlockHeight expired_height)
    : expired_height_(expired_height), committed_size_(0)
  {}

  ReplayCache(std::vector<Uniquifier> uniquifiers, BlockHeight expired_height)
    : uniquifiers_(std::move(uniquifiers)), expired_height_(expired_height)
  {
    filter_expired();
    committed_size_ = uniquifiers_.size();
  }

  bool contains(Uniquifier uniquifier) const;
  bool can_accept(Uniquifier uniquifier) const;

  void add(Uniquifier uniquifier);
  void commit_add(Uniquifier uniquifier);
  void remove(Uniquifier uniquifier);

  std::vector<Uniquifier> const &uniquifiers() const { return uniquifiers_; }

  size_t committed_size() const { return committed_size_; }

  BlockHeight expired_height() const { return expired_height_; }

private:
  std::vector<Uniquifier> uniquifiers_;
  BlockHeight expired_height_;

  size_t committed_size_;

  void filter_expired();
};

} // namespace hogd
