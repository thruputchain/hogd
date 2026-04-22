#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "x23/x23.h"

namespace hogd {

class BorrowedContractBatch {
public:
  using value_type = Contract;
  using hash_set_t = std::unordered_set<SHA256Hash>;
  using contract_map_t =
      std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>>;

  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using iterator_concept = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = Contract;
    using reference = const Contract &;
    using pointer = const Contract *;
    using hash_iterator = hash_set_t::const_iterator;

    const_iterator() = default;
    const_iterator(hash_iterator it, hash_iterator end, contract_map_t const &map)
      : it_(it), end_(end), map_(&map)
    {
      skip_missing();
    }

    reference operator*() const
    {
      auto found = map_->find(*it_);
      assert(found != map_->end());
      assert(found->second != nullptr);
      return *found->second;
    }
    pointer operator->() const
    {
      auto found = map_->find(*it_);
      assert(found != map_->end());
      assert(found->second != nullptr);
      return found->second.get();
    }
    const_iterator &operator++()
    {
      ++it_;
      skip_missing();
      return *this;
    }
    const_iterator operator++(int)
    {
      const_iterator out(*this);
      ++(*this);
      return out;
    }

    friend bool operator==(const_iterator const &a, const_iterator const &b)
    {
      return a.it_ == b.it_;
    }

  private:
    void skip_missing()
    {
      while (it_ != end_ && !map_->contains(*it_)) {
        ++it_;
      }
    }

    hash_iterator it_{};
    hash_iterator end_{};
    contract_map_t const *map_ = nullptr;
  };

  BorrowedContractBatch(hash_set_t const &hashes, contract_map_t const &contracts)
    : hashes_(&hashes), contracts_(&contracts)
  {}

  static constexpr size_t max_size() noexcept
  {
    return NewContractBatch::max_size();
  }
  size_t size() const
  {
    size_t out = 0;
    for (auto const &hash : *hashes_) {
      if (contracts_->contains(hash)) {
        ++out;
      }
    }
    return out;
  }
  const_iterator begin() const
  {
    return const_iterator(hashes_->begin(), hashes_->end(), *contracts_);
  }
  const_iterator end() const
  {
    return const_iterator(hashes_->end(), hashes_->end(), *contracts_);
  }

private:
  hash_set_t const *hashes_;
  contract_map_t const *contracts_;
};

} // namespace hogd

namespace x23 {

template<>
struct xdr_meta<hogd::BorrowedContractBatch> {
  static constexpr XdrCategory category = XdrCategory::kVec;
};

template<>
struct serialize_equiv<hogd::NewContractBatch, hogd::BorrowedContractBatch>
  : std::true_type {};

} // namespace x23
