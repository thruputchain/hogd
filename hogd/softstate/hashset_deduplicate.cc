#include "hogd/softstate/hashset_deduplicate.h"
#include "hogd/softstate/atomic_hashset_node.h"

#include <cassert>
#include <exception>
#include <mutex>
#include <unordered_set>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

bool
HashsetDeduplicatorSimple::try_insert(SHA256Hash const &entry)
{
  std::lock_guard lock(mtx_);

  if (set_.contains(entry)) {
    return false;
  }

  set_.insert(entry);
  return true;
}

void
HashsetDeduplicatorSimple::remove(SHA256Hash const &entry)
{
  std::lock_guard lock(mtx_);

  [[maybe_unused]]
  auto res = set_.erase(entry);

  assert(res == 1);
}

bool
HashsetDeduplicatorSimpleV2::try_insert(SHA256Hash const &entry)
{

  InternalEntry internal_entry{.hash_ = entry,
                               .cached_shorthash_ = hasher_(entry)};

  std::lock_guard lock(mtx_);

  if (set_.contains(internal_entry)) {
    return false;
  }

  set_.insert(internal_entry);
  return true;
}

void
HashsetDeduplicatorSimpleV2::remove(SHA256Hash const &entry)
{
  InternalEntry internal_entry{.hash_ = entry,
                               .cached_shorthash_ = hasher_(entry)};

  std::lock_guard lock(mtx_);

  [[maybe_unused]]
  auto res = set_.erase(internal_entry);

  assert(res == 1);
}

bool
AtomicHashsetDeduplicator::try_insert(SHA256Hash const &entry)
{
  AtomicHashsetNode *cur_node = &root_;

  const uint64_t prefix = hasher_(entry);

  while (true) {
    if (cur_node->prefix_len_ == AtomicHashsetNode::kMaxPrefixLen) {
      return cur_node->leaf_handler_->try_insert(entry);
    }

    uint8_t bb = cur_node->get_branch_bits(prefix);

    auto *child = cur_node->get_child(bb);

    if (child == nullptr) {
      auto *new_node =
          new AtomicHashsetNode(prefix, AtomicHashsetNode::kMaxPrefixLen);

      if (cur_node->try_compare_exchange_child(bb, child, new_node)) {
        new_node->commit_owning();
        cur_node = new_node;
        continue;
      }
      delete new_node;
      cpu_relax();
      continue;
    }

    uint8_t child_match_len = child->get_prefix_match_len(prefix);

    if (child->prefix_len_ == child_match_len) {
      // full match
      cur_node = child;
      continue;
    }

    auto *new_node = new AtomicHashsetNode(prefix, child_match_len);
    uint8_t child_bb = new_node->get_branch_bits(child->prefix_);
    AtomicHashsetNode *expect_null = nullptr;

    if (!new_node->try_compare_exchange_child(child_bb, expect_null, child)) {
      std::terminate();
    }

    if (cur_node->try_compare_exchange_child(bb, child, new_node)) {
      new_node->commit_owning();
      cur_node = new_node;
      continue;
    }
    delete new_node;
    cpu_relax();
  }
}

void
AtomicHashsetDeduplicator::remove(SHA256Hash const &entry)
{
  AtomicHashsetNode *cur_node = &root_;

  const uint64_t prefix = hasher_(entry);

  while (true) {
    if (cur_node->prefix_len_ == AtomicHashsetNode::kMaxPrefixLen) {
      cur_node->leaf_handler_->remove(entry);
      return;
    }

    uint8_t bb = cur_node->get_branch_bits(prefix);

    cur_node = cur_node->get_child(bb);

    assert(!!cur_node);
  }
}

} // namespace hogd
