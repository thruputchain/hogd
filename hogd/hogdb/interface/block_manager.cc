#include "hogd/hogdb/interface/block_manager.h"

#include <cassert>
#include <map>
#include <memory>
#include <vector>

#include "hogd/base/hashmap.h"
#include "rpc_types.hh"

namespace hogd {

// note: The first block after reloading should be some block id that is
// already persisted to the database. Creating a separate StorageView for
// this means acquiring an extra shared_lock. This could be avoided, in
// principle, at the cost of much more code complexity. This edge case
// should disappear by the next block.
BlockManager::BlockManager(BlockID root_id,
                           std::shared_ptr<BlockState> initial_state)
  : block_cache_(8) // arbitrary init parameters
    ,
    last_committed_id_(root_id)
{
  assert(initial_state);
  initial_state->assert_closed();
  block_cache_.emplace(std::as_bytes(std::span(&last_committed_id_, 1)),
                       last_committed_id_, std::move(initial_state));
}

void
BlockManager::commit_block(BlockID id)
{
  assert(id != last_committed_id_);
  // TODO(geoff): put last_committed onto a queue
  // of elements that can be committed to DB and then removed.
  auto children_to_remove = block_cache_.modify_element(
      std::as_bytes(std::span(&last_committed_id_, 1)),
      [id](Block &last_committed) {
        last_committed.closed_to_new_children_ = true;

        last_committed.block_data_->assert_closed();

        std::vector<BlockID> out = std::move(last_committed.children_);
        last_committed.children_ = {id};

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

        return out;
      });

  assert(children_to_remove.has_value());

  for (auto remove_id : *children_to_remove) {
    remove_subtree(remove_id);
  }
  last_committed_id_ = id;
}

void
BlockManager::remove_subtree(BlockID root)
{
  std::vector<BlockID> queue{root};

  while (!queue.empty()) {
    auto id = queue.back();
    queue.pop_back();

    std::printf("remove_subtree block %llu\n", id);

    auto key = std::as_bytes(std::span(&id, 1));

    [[maybe_unused]]
    auto res = block_cache_.modify_element(key, [&queue](auto &v) {
      queue.insert(queue.end(), v.children_.begin(), v.children_.end());
      v.closed_to_new_children_ = true;
    });
    assert(res);

    res = block_cache_.erase(key);
    assert(res);
  }
}

std::shared_ptr<BlockState>
BlockManager::get_block_data(BlockID id)
{
  auto res = block_cache_.modify_element(std::as_bytes(std::span(&id, 1)),
                                         [](auto &v) { return v.block_data_; });
  if (res)
    return *res;
  return nullptr;
}

bool
BlockManager::add_child(BlockID parent_id, BlockID new_id,
                        std::shared_ptr<BlockState> block_state)
{
  // first add the new block

  [[maybe_unused]]
  bool insert_res = block_cache_.emplace(std::as_bytes(std::span(&new_id, 1)),
                                         new_id, std::move(block_state));

  // if this assertion fails, then new_id has been used before.
  assert(insert_res);

  auto link_res =
      block_cache_.modify_element(std::as_bytes(std::span(&parent_id, 1)),
                                  [new_id](auto &parent) -> bool {
                                    if (parent.closed_to_new_children_) {
                                      return false;
                                    }

                                    parent.children_.push_back(new_id);
                                    return true;
                                  });

  if (link_res.has_value() && *link_res) {
    // successfully linked to parent
    return true;
  }

  // garbage collect
  [[maybe_unused]]
  auto erase_res = block_cache_.erase(std::as_bytes(std::span(&new_id, 1)));
  assert(erase_res);
  return false;
}

} // namespace hogd
