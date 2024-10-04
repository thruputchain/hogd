#pragma once


#include <mutex>
#include <vector>
#include <cstdint>

#include "x23/mvar.h"

#include "types.hh"

namespace hogd {

class LeafCollisionHandler {
public:

	bool try_insert(SHA256Hash const& entry);
	void remove(SHA256Hash const& entry);

private:
	x23::MVar<std::vector<SHA256Hash>> hashes_;
};

class AtomicHashsetNode
{
public:
	constexpr static uint16_t kMaxPrefixLen = 16;
	constexpr static uint8_t kBranchBits = 4;

	AtomicHashsetNode(uint64_t prefix, uint8_t prefix_len);

	~AtomicHashsetNode();

	uint8_t get_prefix_match_len(uint64_t other_prefix) const;
	uint8_t get_branch_bits(uint64_t other_prefix) const;

	AtomicHashsetNode* get_child(uint8_t branch_bits) const {
		return children_[branch_bits].load(std::memory_order_acquire);
	}

	// sets new_pointer to current value if swap fails
	bool try_compare_exchange_child(uint8_t branch_bits, AtomicHashsetNode*& expect, AtomicHashsetNode* desired) {
		return children_[branch_bits].compare_exchange_strong(expect, desired, std::memory_order_release);
	}

	void commit_owning() {
		owning_ = true;
	}

	const uint64_t prefix_;
	const uint8_t prefix_len_;

	std::unique_ptr<LeafCollisionHandler> leaf_handler_;

private:

	bool owning_ = false;

	using children_t = std::array<std::atomic<AtomicHashsetNode*>, 16>;

	children_t children_;

	static_assert(std::tuple_size_v<children_t> == 1 << kBranchBits);
};


}