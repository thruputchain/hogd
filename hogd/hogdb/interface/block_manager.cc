#include "hogd/hogdb/interface/block_manager.h"

#include <cassert>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/hashmap.h"
#include "hogd/hogdb/db/in_memory_database.h"
#include "hogd/hogdb/softstate/block_state.h"
#include "rpc_types.hh"

namespace hogd {

// note: The first block after reloading should be some block id that is
// already persisted to the database. Creating a separate StorageView for
// this means acquiring an extra shared_lock. This could be avoided, in
// principle, at the cost of much more code complexity. This edge case
// should disappear by the next block.
BlockManager::BlockManager(BlockID root_id,
                           std::shared_ptr<BlockState> initial_state,
                           InMemoryDatabase *base_database)
  : block_cache_(8), last_committed_id_(root_id), base_database_(base_database)
{
  assert(initial_state);
  initial_state->assert_closed();
  assert(initial_state->is_committed_to_database());
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

  auto children_to_unlink = block_cache_.modify_element(
      std::as_bytes(std::span(&last_committed_id_, 1)), [](auto &v) {
        v.block_data_->commit_to_database();
        return v.children_;
      });
  assert(children_to_unlink);

  for (auto child_to_unlink : *children_to_unlink) {
    // Child might get removed via a commit of a sibling.  That's ok,
    // we don't have to bother unlinking parent in that case.
    block_cache_.modify_element(std::as_bytes(std::span(&child_to_unlink, 1)),
                                [this](auto &v) {
                                  v.block_data_->unlink_parent(base_database_);
                                });
  }
}

void
BlockManager::remove_subtree(BlockID root)
{
  std::vector<BlockID> queue{root};

  while (!queue.empty()) {
    auto id = queue.back();
    queue.pop_back();

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
  assert(block_state);
  // first add the new block

  [[maybe_unused]]
  bool insert_res = block_cache_.emplace(std::as_bytes(std::span(&new_id, 1)),
                                         new_id, std::move(block_state));

  // if this assertion fails, then new_id has been used before.
  assert(insert_res);

  auto link_res = block_cache_.modify_element(
      std::as_bytes(std::span(&parent_id, 1)),
      [new_id](auto &parent) -> std::pair<bool, bool> {
        bool parent_is_committed =
            parent.block_data_->is_committed_to_database();

        if (parent.closed_to_new_children_) {
          return {false, parent_is_committed};
        }

        parent.children_.push_back(new_id);
        return {true, parent_is_committed};
      });
  // Just to document that edge cases were considered:
  // What if we add the child, and at that time the parent is
  // not committed to the database, but then the parent is committed?
  // In that case, though, the child is already linked in parent.children_,
  // so the caller of the commit on the parent will unlink the child.
  //
  // On the other hand, if the parent is already committed, then we need to
  // unlink the new child.
  if (link_res.has_value() && link_res->first) {
    // successfully linked to parent

    if (link_res->second) {
      // might not exist, if a sibling is concurrently removed,
      // but that's ok.
      block_cache_.modify_element(std::as_bytes(std::span(&new_id, 1)),
                                  [this](auto &v) {
                                    v.block_data_->unlink_parent(
                                        base_database_);
                                  });
    }

    return true;
  }

  // garbage collect
  [[maybe_unused]]
  auto erase_res = block_cache_.erase(std::as_bytes(std::span(&new_id, 1)));
  assert(erase_res);
  return false;
}

} // namespace hogd
