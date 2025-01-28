#pragma once

#include "hogd/coord/block_frame.h"
#include "hogd/coord/database_manager.h"
#include "rpc_types.hh"

namespace hogd {

enum class CoordBlockState {
  kOpenForProposal,
  kProposalClosed,
  kValidating,
  kValidateSuccess,
  kValidateFailure,
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

private:
  std::variant<BlockProposalFrame, BlockValidationFrame> current_frame_;
  const BlockID block_id_;
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
