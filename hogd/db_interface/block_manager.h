
#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "hogd/db_interface/block_state.h"
#include "rpc_types.hh"

// Data structures to manage a possibly-forking tree of blocks.
// We might need something like this in the coordinator.
// Could probably be abstracted into a template if necessary.

namespace hogd {

class InMemoryDatabase;

class BlockManager {
public:
  // root_id is the id of the block from which db is initialized.
  BlockManager(BlockID root_id, InMemoryDatabase *base_database);

  // Requirement: The supplied id must be a direct child of the most recently
  // committed block. Deletes from block_cache_ any entries that might not
  // commit in the future. (i.e. any siblings of the supplied id, and any of
  // their children).
  // There should not be concurrent commits of any block.
  //
  // TODO(geoff): This does not clean up the entry for the parent
  // committed block.  This might be a reasonable thing to do.  Depends on
  // whether there's any part of the state that needs to get kept around for a
  // bit longer. Could be made a part of this, or could be a separate rpc.
  void commit_block(BlockID id);

  // returns nullptr if id is not active
  std::shared_ptr<BlockState> get_block_data(BlockID id);

  // new_id should be a new, never-before-used ID.
  // returns true if new_id is successfully linked to the parent.
  // Otherwise the parent id is no longer a valid block to link to.
  bool add_child(BlockID parent_id, BlockID new_id);

private:
  // One block.
  // Does not own a reference to children.
  // If this block commits, then eventually, one of its children will also
  // commit. Only one will commit; the others will be garbage collected.
  struct Block {

    // ctor for block that is already committed to database
    Block() = default;

    // ctor for block with a parent that has not committed to database.
    Block(BlockState *parent_state)
      : block_data_(std::make_shared<BlockState>(parent_state))
    {}

    // ctor for block whose parent has already committed to database.
    Block(InMemoryDatabase *base_db)
      : block_data_(std::make_shared<BlockState>(base_db))
    {}

    bool block_is_closed() const
    {
      if (!block_data_) {
        // if block_data_ is nullptr, then the block is committed to database
        // (and thus is closed).
        return true;
      }
      return block_data_->is_closed();
    }

    bool closed_to_new_children_ = false;
    std::vector<BlockID> children_;

    // Object can give out shared_ptr to block_data_.
    // Useful if e.g. an RPC requests information relevant to a block,
    // and concurrently the block is deleted (if e.g. a sibling commits).
    std::shared_ptr<BlockState> block_data_;
  };

  // remove the block, and all of its children
  // returns the block_data_ for each block that was removed;
  // this lets us push the work of garbage collection outside the critical
  // section of the lock.
  std::vector<std::shared_ptr<BlockState>> remove_subtree(BlockID root);

  std::shared_mutex mtx_;
  // Always contains at least one entry: the most recently committed block.
  std::unordered_map<BlockID, Block> block_cache_;
  BlockID last_committed_id_;
  InMemoryDatabase *const base_database_;
};

} // namespace hogd
