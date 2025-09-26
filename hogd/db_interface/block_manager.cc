#include "hogd/db_interface/block_manager.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "account.hh"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_state.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

// note: The first block after reloading should be some block id that is
// already persisted to the database. Creating a separate StorageView for
// this means acquiring an extra shared_lock. This could be avoided, in
// principle, at the cost of much more code complexity. This edge case
// should disappear by the next block.
BlockManager::BlockManager(BlockID root_id, InMemoryDatabase *base_database)
  : cost_model_(12000, 1, 5, 100, 5), last_committed_id_(root_id),
    base_database_(base_database)
{
  block_cache_.emplace(last_committed_id_, Block());
}

bool
BlockManager::commit_block(BlockID id)
{
  std::vector<std::shared_ptr<BlockState>> blocks_to_gc;

  {
    std::lock_guard lock(mtx_);

    if (!block_cache_.contains(id)) {
      return false;
    }

    if (id == last_committed_id_) {
      return false;
    }

    if (!block_cache_.at(id).block_is_closed()) {
      return false;
    }

    auto descendant_check = [&] {
      auto const &lc = block_cache_.at(last_committed_id_);
      return std::ranges::any_of(lc.children_, [id](auto const &child) {
        return child == id;
      });
    }();
    if (!descendant_check) {
      return false;
    }
    // TODO(geoff): In a real impl this should definitely not be on the critical
    // path, but this won't be a real implementation (pending abelay's
    // replacement of this)
    block_cache_.at(id).block_data_->commit_to_database();

    // TODO(geoff): consider putting last_committed onto a queue
    // of elements that can be committed to DB and then removed.

    auto children_to_remove = [&] {
      auto it = block_cache_.find(last_committed_id_);
      assert(it != block_cache_.end());

      auto &last_committed_block = it->second;
      last_committed_block.closed_to_new_children_ = true;

      std::vector<BlockID> out = std::move(last_committed_block.children_);
      last_committed_block.children_ = {id};

      [[maybe_unused]]
      bool committed_child_found = false;
      for (auto &child : out) {
        if (child == id) {
          child = out.back();
          out.pop_back();
          committed_child_found = true;
          break;
        }
      }
      assert(committed_child_found);
      block_cache_.erase(it);
      return out;
    }();

    for (auto remove_id : children_to_remove) {
      auto r = remove_subtree(remove_id);
      blocks_to_gc.insert(blocks_to_gc.end(), r.begin(), r.end());
    }
    last_committed_id_ = id;

    // Now unlink all the children of the new last committed block,
    // telling them to read from the database as the state parent.
    auto it = block_cache_.find(last_committed_id_);
    assert(it != block_cache_.end());

    for (auto &child : it->second.children_) {
      block_cache_.at(child).block_data_->unlink_parent(base_database_);
    }
  }
  // redundant with dtor, but to be explicit
  blocks_to_gc.clear();
  return true;
}

std::vector<std::shared_ptr<BlockState>>
BlockManager::remove_subtree(BlockID root)
{
  std::vector<BlockID> queue{root};
  // push the garbage collection outside the lock critical section
  std::vector<std::shared_ptr<BlockState>> blocks_to_gc;

  while (!queue.empty()) {
    auto id = queue.back();
    queue.pop_back();

    auto it = block_cache_.find(id);
    assert(it != block_cache_.end());

    auto &block = it->second;
    blocks_to_gc.emplace_back(std::move(block.block_data_));

    queue.insert(queue.end(), block.children_.begin(), block.children_.end());

    block_cache_.erase(it);
  }
  return blocks_to_gc;
}

std::shared_ptr<BlockState>
BlockManager::get_block_data(BlockID id)
{
  std::shared_lock lock(mtx_);
  auto it = block_cache_.find(id);
  if (it == block_cache_.end()) {
    return nullptr;
  }
  return it->second.block_data_;
}

bool
BlockManager::add_child(BlockID parent_id, BlockID new_id,
                        BlockExecutionMode mode)
{
  // first add the new block
  std::lock_guard lock(mtx_);

  // parent has already been gc'd or committed to db before last_committed_id_
  auto parent_it = block_cache_.find(parent_id);
  if (parent_it == block_cache_.end()) {
    return false;
  }

  auto &parent = parent_it->second;
  if (parent.closed_to_new_children_) {
    return false;
  }

  if (block_cache_.contains(new_id)) {
    // new_id is already active
    return false;
  }

  if (!parent.block_is_closed()) {
    // can't have a child block until the parent is closed
    return false;
  }

  // parent exists, and is not closed to new children
  // so we can add the new child

  // add the new child id to the parent's children before
  // actually inserting the new element (and invalidating parent_it)
  parent.children_.push_back(new_id);

  if (!parent.block_data_ || parent.block_data_->is_committed_to_database()) {
    // parent is committed to database, so we need to create a new block state
    block_cache_.emplace(new_id, Block(base_database_, mode, &cost_model_));
  }
  else {
    assert(parent_id != last_committed_id_);
    // parent is not committed to database, so we can use the parent's block
    // state
    block_cache_.emplace(new_id,
                         Block(parent.block_data_.get(), mode, &cost_model_));
  }
  return true;
}

bool
BlockManager::initialize_genesis_block(GenesisBlock const &config)
{
  std::lock_guard lock(mtx_);
  if (initialized_genesis_) {
    return false;
  }
  initialized_genesis_ = true;

  GenesisBlockLoader loader(config);

  loader.load_genesis_block(
      [&](StorageKey const &key, int128_t value) -> int128_t {
        auto item = std::make_shared<StorageItemWrapper>(value, &cost_model_);
        base_database_->write_item(
            AddressAndKey{.address = GenesisBlockLoader::kDeployContractAddress,
                          .key = key},
            item);
        return item->storage_cost();
      },
      [&](std::shared_ptr<const Contract> const &contract) {
        assert(contract);
        auto hash = sha256(*contract);
        base_database_->write_contract(hash, contract);
      },
      [&](const ContractAddress &addr, const DeployedContractHash &hash) {
        auto item =
            std::make_shared<const StorageItemWrapper>(hash, &cost_model_);
        base_database_->write_item(
            AddressAndKey{.address = addr,
                          .key = StorageKey(
                              StorageKeyType::kStorageKeyTypeAddressMetadata)},
            item);
      });

  return true;
}

} // namespace hogd
