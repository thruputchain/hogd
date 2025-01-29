#pragma once

#include "hogd/coord/block_frame.h"
#include "hogd/coord/database_manager.h"
#include "rpc_types.hh"

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
  CoordBlock(BlockID block_id, std::vector<ProposedTransaction> txs);

  bool ready_for_commit() const;
  bool committed() const { return state_ == CoordBlockState::kCommitted; }
  void mark_as_committed()
  {
    assert(!committed());
    state_ = CoordBlockState::kCommitted;
  }

  const BlockID block_id_;

private:
  std::variant<BlockProposalFrame, BlockValidationFrame> current_frame_;
  CoordBlockState state_;
};

class CoordinatorBlockManager {
public:
  CoordinatorBlockManager(DatabaseManager &database_manager);

  std::optional<BlockID> open_proposal(BlockID parent_id);
  std::optional<BlockID>
  add_block_to_validate(BlockID parent_id,
                        std::vector<ProposedTransaction> txs);
  void commit_block(BlockID block_id);

  std::shared_ptr<CoordBlock> get_block(BlockID block_id);

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
  // TODO(geoff): initialize this with the same id the database is using
  // We could just keep this at 0, probably.  Depends on db load from disk code.
  BlockID last_committed_block_id_ = 0;
  BlockID next_block_id_ = last_committed_block_id_ + 1;
};

} // namespace hogd
