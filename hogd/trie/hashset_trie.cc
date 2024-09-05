#include "hogd/trie/hashset_trie.h"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "hogd/crypto/blake2b.h"
#include "hogd/trie/prefix.h"

namespace hog {

HashsetTrieNode::HashsetTrieNode(prefix_t const &prefix,
                                 PrefixLenBits const &prefix_len)
  : prefix_(prefix), prefix_len_(prefix_len)
{
  prefix_.truncate(prefix_len_);
}

void
HashsetTrieNode::clear_hash()
{
  auto *ptr = hash_.load(std::memory_order_relaxed);
  if (ptr != nullptr) {
    hash_.store(nullptr, std::memory_order_relaxed);
    delete ptr;
  }
}

uint8_t
HashsetTrieNode::get_branch_bits(prefix_t const &other) const
{
  return other.get_branch_bits(prefix_len_);
}

void
HashsetTrieNode::set_as_non_owner()
{
  owns_children_.store(false, std::memory_order_relaxed);
}

PrefixLenBits
HashsetTrieNode::get_prefix_match_len(prefix_t const &other) const
{
  return prefix_.get_prefix_match_len(prefix_len_, other, prefix_t::kMaxLen);
}

bool
HashsetTrie::insert(uint64_t clear_threshold, Hash const &hash)
{
  const auto element = HashsetTrieNode::make_prefix(clear_threshold, hash);

  auto *current_node_visitor = &root_;

  HashsetTrieNode *work_buffer = nullptr;

  while (true) {
    current_node_visitor->clear_hash();

    if (current_node_visitor->prefix_len_ ==
        HashsetTrieNode::prefix_t::kMaxLen) {
      assert(current_node_visitor->prefix_ == element);

      delete work_buffer;

      return false;
    }

    assert(current_node_visitor->get_prefix_match_len(element) ==
           current_node_visitor->prefix_len_);

    auto bb = current_node_visitor->get_branch_bits(element);

    auto &child_slot = current_node_visitor->children_[bb];

    auto *child_ptr = child_slot.load(std::memory_order_relaxed);

    while (true) {
      if (child_ptr == nullptr) {
        if (!work_buffer) {
          work_buffer =
              new HashsetTrieNode(element, HashsetTrieNode::prefix_t::kMaxLen);
        }
        else {
          work_buffer = new (work_buffer)
              HashsetTrieNode(element, HashsetTrieNode::prefix_t::kMaxLen);
        }

        if (child_slot.compare_exchange_strong(child_ptr, work_buffer,
                                               std::memory_order_relaxed)) {
          // work_buffer is accounted for, return
          work_buffer = nullptr;
          return true;
        }
        // Not really necessary, with current setup, but setting anyways
        // to avoid later problems, hopefully
        work_buffer->set_as_non_owner();
        work_buffer->~HashsetTrieNode();
        // SPINLOCK_PAUSE();
        continue; // contention: failed to set new_child -- retry
      }

      auto child_match_len = child_ptr->get_prefix_match_len(element);

      if (child_match_len == child_ptr->prefix_len_) {
        current_node_visitor = child_ptr;
        break; // success: found child node extending prefix, continue
      }

      if (!work_buffer) {
        work_buffer = new HashsetTrieNode(element, child_match_len);
      }
      else {
        work_buffer =
            new (work_buffer) HashsetTrieNode(element, child_match_len);
      }

      auto child_bb = work_buffer->get_branch_bits(child_ptr->prefix_);
      work_buffer->children_[child_bb].store(child_ptr,
                                             std::memory_order_relaxed);

      if (child_slot.compare_exchange_strong(child_ptr, work_buffer,
                                             std::memory_order_relaxed)) {
        current_node_visitor = work_buffer;
        // child_slot owns work_buffer
        work_buffer = nullptr;
        break; // success: created new intermediate node, continue
      }
      work_buffer->set_as_non_owner();
      work_buffer->~HashsetTrieNode();

      // SPINLOCK_PAUSE();
      // contention: retry loop
    }
  }
}

const HashsetTrieNode::NodeMetadata *
HashsetTrieNode::rehash(std::vector<uint8_t> &workspace)
{
  NodeMetadata *out = get_metadata();
  if (out) {
    return out;
  }

  std::unique_ptr<NodeMetadata> new_hash;

  if (prefix_len_ == prefix_t::kMaxLen) {
    new_hash = std::make_unique<NodeMetadata>();

    hash_blake2b(new_hash->hash, prefix_.underlying_data_ptr(),
                 prefix_t::kMaxLenBytes);

    new_hash->size = 1;
  }
  else {
    for (uint8_t i = 0; i < 16; i++) {
      workspace.resize(2 + 40 + 32 * 16, 0);
      // TODO(geoff) this is kind of brittle and annoying -- is there an easy
      // way that's equivalently fast (i.e. avoiding an xdr::marshal allocation)
      // Hash format: prefix_len [2 bytes, little endian]
      // prefix [40 bytes]
      // children hashes [16 * 32 bytes]

      std::array<const NodeMetadata *, 16> child_hashes;
      for (uint8_t i = 0; i < 16; i++) {
        auto *ptr = children_[i].load(std::memory_order_relaxed);
        if (ptr == nullptr)
          continue;

        child_hashes[i] = ptr->get_metadata();

        assert(child_hashes[i]);

        // TODO(geoff): investigate heuristics like this one
        if (i % 4 == 3) {
          out = get_metadata();
          if (out) {
            return out;
          }
        }
      }

      *reinterpret_cast<uint16_t *>(workspace.data()) = prefix_len_.len;
      std::memcpy(workspace.data() + sizeof(uint16_t),
                  prefix_.underlying_data_ptr(), prefix_t::kMaxLenBytes);
      uint16_t offset = sizeof(uint16_t) + prefix_t::kMaxLenBytes;
      uint32_t sz = 0;
      for (uint8_t i = 0; i < 16; i++) {
        if (child_hashes[i] == nullptr) {
          std::memset(workspace.data() + offset, 0, 32);
        }
        else {
          sz += child_hashes[i]->size;
          std::memcpy(workspace.data() + offset, child_hashes[i]->hash.data(),
                      32);
        }
        offset += 32;
      }

      assert(offset == workspace.size());

      new_hash = std::make_unique<NodeMetadata>();
      new_hash->size = sz;

      hash_blake2b(new_hash->hash, workspace.data(), workspace.size());
    }
  }

  assert(new_hash);

  auto *new_hash_backup = new_hash.get();
  if (hash_.compare_exchange_strong(out, new_hash.get(),
                                    std::memory_order_relaxed)) {
    new_hash.release(); // NOLINT
    return new_hash_backup;
  }
  return out;
}

const HashsetTrieNode::NodeMetadata *
HashsetTrie::guided_rehash(HashsetTrieNode::prefix_t const &target,
                           std::vector<uint8_t> &workspace)
{

  struct RecursionState {
    HashsetTrieNode *node;
    uint8_t idx = 0;
  };

  std::vector<RecursionState> state;

  state.emplace_back(&root_);

  while (!state.empty()) {
    auto &current = state.back();

    if (current.node->get_metadata()) {
      state.pop_back();
      continue;
    }

    if (current.idx == 16) {
      current.node->rehash(workspace);
      state.pop_back();
      continue;
    }

    uint8_t bb = current.node->get_branch_bits(target);
    uint8_t idx = current.idx;

    auto *next_child =
        current.node->children_[(idx ^ bb)].load(std::memory_order_relaxed);

    current.idx++;
    state.emplace_back(next_child);
  }
  auto *out = root_.get_metadata();
  assert(out);

  return out;
}

HashsetTrieNode::prefix_t
HashsetTrieNode::make_prefix(uint64_t clear_threshold, Hash const &elt)
{
  uint64_t big_endian = std::byteswap(clear_threshold);

  // swap nibbles per byte
  constexpr uint64_t kHighNibbles = 0xF0F0'F0F0'F0F0'F0F0;
  constexpr uint64_t kLowNibbles = kHighNibbles >> 4;
  big_endian =
      ((big_endian & kHighNibbles) >> 4) | ((big_endian & kLowNibbles) << 4);

  prefix_t out;
  *reinterpret_cast<uint64_t *>(out.underlying_data_ptr()) = big_endian;

  static_assert(std::is_same_v<decltype(out.underlying_data_ptr()), uint8_t *>,
                "hit this bug by accident before");

  std::memcpy(out.underlying_data_ptr() + sizeof(uint64_t), elt.data(),
              elt.size());

  return out;
}

std::vector<std::unique_ptr<HashsetTrieNode>>
HashsetTrie::remove_clear_threshold(
    HashsetTrieNode::prefix_t const &clear_threshold)
{
  auto *current_node_visitor = &root_;

  std::vector<std::unique_ptr<HashsetTrieNode>> out;

  std::vector<HashsetTrieNode *> visit_stack;

  while (true) {
    visit_stack.push_back(current_node_visitor);

    assert(current_node_visitor->prefix_len_.len < 64);

    current_node_visitor->clear_hash();

    uint8_t bb = current_node_visitor->get_branch_bits(clear_threshold);

    for (uint8_t i = 0; i < bb; i++) {
      auto *extract_child = current_node_visitor->children_[i].exchange(
          nullptr, std::memory_order_relaxed);
      if (extract_child != nullptr) {
        out.emplace_back(extract_child);
      }
    }

    auto *match_child =
        current_node_visitor->children_[bb].load(std::memory_order_relaxed);

    if (match_child == nullptr) {
      break;
    }

    if (match_child->prefix_len_.len < 64) {
      current_node_visitor = match_child;
      continue;
    }

    auto truncate = match_child->prefix_;
    truncate.truncate(64);

    if (truncate <= clear_threshold) {
      current_node_visitor->children_[bb].store(nullptr,
                                                std::memory_order_relaxed);
      out.emplace_back(match_child);
    }
    break;
  }

  current_node_visitor = &root_;

  for (size_t i = 1; i < visit_stack.size(); i++) {
    auto extract = visit_stack[i]->maybe_extract_singlechild();
    if (extract) {
      uint8_t bb = current_node_visitor->get_branch_bits(extract->prefix_);

      assert(current_node_visitor->children_[bb] == visit_stack[i]);

      current_node_visitor->children_[bb].store(extract.get(),
                                                std::memory_order_relaxed);
      extract.release(); // NOLINT

      delete visit_stack[i];
    }
    else {
      current_node_visitor = visit_stack[i];
    }
  }

  return out;
}

std::unique_ptr<HashsetTrieNode>
HashsetTrieNode::maybe_extract_singlechild()
{
  HashsetTrieNode *unique_child = nullptr;
  uint8_t idx = UINT8_MAX;
  for (uint8_t i = 0; i < 16; i++) {
    auto *child = children_[i].load(std::memory_order_relaxed);
    if (child != nullptr) {
      if (unique_child == nullptr) {
        unique_child = child;
        idx = i;
      }
      else {
        return nullptr;
      }
    }
  }
  if (idx == UINT8_MAX) {
    return nullptr;
  }
  children_[idx].store(nullptr, std::memory_order_relaxed);
  return std::unique_ptr<HashsetTrieNode>(unique_child);
}

HashsetTrieNode::~HashsetTrieNode()
{
  if (owns_children_.load(std::memory_order_relaxed)) {
    for (uint8_t i = 0; i < 16; i++) {
      // delete nullptr has no effect
      delete children_[i].load(std::memory_order_relaxed);
    }
  }

  delete hash_.load(std::memory_order_relaxed);
}

} // namespace hog
