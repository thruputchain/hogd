#pragma once

#include "hogd/coord/block_frame.h"
#include "hogd/coord/database_manager.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace hogd {

// TODO(geoff): add systems to remove failed validation blocks at failure time.
// They're already gc'd whenever a sibling block commits, so this is not high
// priority.

enum class CoordBlockState {
  // Block is accepting new transactions for proposal
  kOpenForProposal,
  // Block proposal phase is complete, no new transactions accepted
  kProposalClosed,
  // Block is being validated by validators
  kValidating,
  // Block validation completed successfully
  kValidateSuccess,
  // Block validation failed
  kValidateFailure,
  // Block has been committed to the database
  kCommitted
};

class CoordBlock {
public:
  CoordBlock(BlockID block_id);

  // validation ctor
  CoordBlock(BlockID block_id, std::vector<ProposedTransaction> txs,
             SHA256Hash block_hash);

  bool ready_for_commit() const;

  void mark_validation_success()
  {
    assert(state_ == CoordBlockState::kValidating);
    state_ = CoordBlockState::kValidateSuccess;
  }

  void mark_validation_failed()
  {
    assert(state_ == CoordBlockState::kValidating);
    state_ = CoordBlockState::kValidateFailure;
  }

  bool committed() const { return state_ == CoordBlockState::kCommitted; }
  void mark_as_committed()
  {
    assert(!committed());
    state_ = CoordBlockState::kCommitted;
  }

  BlockValidationFrame *get_validation_frame()
  {
    assert(std::holds_alternative<BlockValidationFrame>(current_frame_));
    return &std::get<BlockValidationFrame>(current_frame_);
  }

  SHA256Hash canonical_block_hash() const;

  const BlockID block_id_;

private:
  std::variant<BlockProposalFrame, BlockValidationFrame> current_frame_;
  std::atomic<CoordBlockState> state_;
};

class CoordinatorBlockManager {
public:
  CoordinatorBlockManager(DatabaseManager &database_manager);

  std::optional<BlockID> open_proposal(BlockID parent_id);
  std::optional<BlockID>
  add_block_to_validate(BlockID parent_id, std::vector<ProposedTransaction> txs,
                        SHA256Hash const &block_hash);

  constexpr static SHA256Hash kGenesisBlockHash = SHA256Hash{};

  // Each block should be either pruned or closed.
  // Do not call both.
  //
  // Marks a proposal as closed, and returns the root hash.
  // Marks a validation as finished successfully (meaning the hash
  // supplied with the block should be verified to be correct).
  x23::Async<std::optional<SHA256Hash>> close_block(BlockID block_id);
  // Remove a block that either failed validation or a proposal that was
  // cancelled.
  void prune_block(BlockID block_id);

  void commit_block(BlockID block_id);

  std::shared_ptr<CoordBlock> get_block(BlockID block_id);

  // nullopt if the block has not beed closed.
  std::optional<BlockID> lookup_block_id(SHA256Hash hash) const;

private:
  struct BlockEntry {
    std::shared_ptr<CoordBlock> block_;
    std::vector<BlockID> children_;
    bool closed_to_new_children_ = false;
  };

  BlockID allocate_block_id() { return next_block_id_++; }
  std::unordered_map<BlockID, BlockEntry>::iterator
  open_block_guard(BlockID parent_id);
  void remove_children(std::vector<BlockID> children);

  DatabaseManager &database_manager_;

  std::mutex mtx_;
  std::unordered_map<BlockID, BlockEntry> blocks_;
  // populated when a block is marked as validated/closed.
  std::unordered_map<SHA256Hash, BlockID> block_hash_to_id_;

  // TODO(geoff): initialize this with the same id the database is using
  // We could just keep this at 0, probably.  Depends on db load from disk code.
  BlockID last_committed_block_id_ = 0;
  BlockID next_block_id_ = last_committed_block_id_ + 1;
};

} // namespace hogd
