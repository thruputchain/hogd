#pragma once

#include "hogd/base/hashmap.h"

#include "rpc_types.hh"

// Data structures to manage a possibly-forking tree of blocks.
// We might need something like this in the coordinator.
// Could probably be abstracted into a template if necessary.

namespace hogd {

// Placeholder
struct BlockState {};

class BlockManager {
public:
  // root_id is the id of the block from which db is initialized.
  // TODO(geoff): This should be determined by some process in crash recovery
  // code. That initialization process should also provide block_data_ for that
  // committed block.
  BlockManager(BlockID root_id, std::shared_ptr<BlockState> initial_state);

  // Requirement: The supplied id must be a direct child of the most recently
  // committed block. Deletes from block_cache_ any entries that might not
  // commit in the future. (i.e. any siblings of the supplied id, and any of
  // their children).
  void commit_block(BlockID id);

  std::shared_ptr<BlockState> get_block_data(BlockID id);

  // new_id should be a new, never-before-used ID.
  // returns true if new_id is successfully linked to the parent.
  // Otherwise the parent id is no longer a valid block to link to.
  bool add_child(BlockID parent_id, BlockID new_id,
                 std::shared_ptr<BlockState> block_state);

private:
  // One block.
  // Does not own a reference to children.
  // If this block commits, then eventually, one of its children will also
  // commit. Only one will commit; the others will be garbage collected.
  struct Block {
    Block(BlockID id, std::shared_ptr<BlockState> data)
      : id_(id), children_(), block_data_(std::move(data))
    {}

    std::span<const std::byte> get_key() const
    {
      return std::as_bytes(std::span(&id_, 1));
    }

    BlockID id_;
    bool closed_to_new_children_ = false;
    std::vector<BlockID> children_;

    // Object can give out shared_ptr to block_data_.
    // Useful if e.g. an RPC requests information relevant to a block,
    // and concurrently the block is deleted (if e.g. a sibling commits).
    //
    // A parent may get garbage collected before its children.
    // (TODO(geoff): we could be careful during remove_subtree to prevent this).
    // As such, it may be necessary for a child to hold a shared_ptr to its
    // parent's block_data_.
    std::shared_ptr<BlockState> block_data_;
  };

  // remove the block, and all of its children
  void remove_subtree(BlockID root);

  HashMap<Block> block_cache_;

  BlockID last_committed_id_;
};

} // namespace hogd
