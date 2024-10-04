#include "hogd/softstate/atomic_hashset_node.h"

namespace hogd {


bool 
LeafCollisionHandler::try_insert(SHA256Hash const& entry)
{
	auto vec = hashes_.take();
	for (auto const& value : *vec) {
		if (entry == value) {
			return false;
		}
	}
	vec->emplace_back(entry);
	return true;
}

void 
LeafCollisionHandler::remove(SHA256Hash const& entry)
{
	auto vec = hashes_.take();

	for (auto it = vec->begin(); it != vec->end(); it++) {
		if (entry == *it) {
			vec->erase(it);
			return;
		}
	}
	throw std::runtime_error("invalid remove");
}

AtomicHashsetNode::AtomicHashsetNode(uint64_t prefix, uint8_t prefix_len)
	: prefix_(prefix)
	, prefix_len_(prefix_len)
	, owning_(false)
{
	if (prefix_len_ == kMaxPrefixLen) {
		leaf_handler_ = std::make_unique<LeafCollisionHandler>();
	}
}


uint8_t 
AtomicHashsetNode::get_prefix_match_len(uint64_t other_prefix) const
{
  uint64_t diff = other_prefix ^ prefix_;

  if (diff == 0) {
    return prefix_len_;
  }

  uint8_t out = std::min<uint8_t>(prefix_len_, std::countr_zero(diff));
  out -= (out % kBranchBits);

  return out;
}


uint8_t 
AtomicHashsetNode::get_branch_bits(uint64_t other_prefix) const {
	return (other_prefix >> prefix_len_) & 0xF;
}


AtomicHashsetNode::~AtomicHashsetNode() {
	if (owning_) {
		for (auto& ptr : children_) {
			delete ptr;
		}
	}
}

}

