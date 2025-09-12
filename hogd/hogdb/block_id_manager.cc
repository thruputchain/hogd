#include "hogd/hogdb/block_id_manager.h"

#include <format>
#include <mutex>
#include <stdexcept>
#include <string_view>

#include "hogd/base/log.h"
#include "rpc_types.hh"

namespace hogd {

namespace {
template<typename... Args>
void
log_error(std::string_view message, Args &&...args)
{
  auto str = std::vformat(message, std::make_format_args(args...));
  error("{}", str);
  if (BlockIDStateManager::throw_on_error_) {
    throw std::runtime_error(str);
  }
}
} // namespace

void
BlockIDStateManager::log_block_id_tip(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (last_committed_block_id_) {
    log_error("BlockIDStateManager: last_committed_block_id_ is already set "
              "(double initialization)");
    return;
  }
  last_committed_block_id_ = block_id;
}

void
BlockIDStateManager::log_block_open(BlockID parent_id, BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!last_committed_block_id_) {
    log_error("BlockIDStateManager: last_committed_block_id_ is not set (block "
              "open before initialization)");
    return;
  }
  if (parent_id != last_committed_block_id_) {
    log_error(
        "BlockIDStateManager: parent id {} is not the last committed block "
        "id {}",
        parent_id, *last_committed_block_id_);
    return;
  }
  if (block_id_states_.contains(block_id)) {
    log_error("BlockIDStateManager: block id {} is already open (double open)",
              block_id);
    return;
  }
  if (block_id <= *last_committed_block_id_) {
    // Technically all we need is that block ids are unique (and even then after
    // they commit/abandon we could in principle reuse them) but this strict
    // greater than check is (1) easy to enforce and (2) banning blockid
    // recycling makes debugging easier
    log_error(
        "BlockIDStateManager: block id {} is not strictly greater than the "
        "last committed block id {}",
        block_id, *last_committed_block_id_);
    return;
  }
  block_id_states_[block_id] = BlockIDState::kOpened;
}

void
BlockIDStateManager::log_block_close(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = block_id_states_.find(block_id);
  if (it == block_id_states_.end()) {
    log_error("BlockIDStateManager: closing a block id {} that does not exist",
              block_id);
    return;
  }
  if (it->second != BlockIDState::kOpened) {
    log_error("BlockIDStateManager: closing a block id {} that is not open",
              block_id);
    return;
  }
  it->second = BlockIDState::kClosed;
}

void
BlockIDStateManager::log_block_commit(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = block_id_states_.find(block_id);
  if (it == block_id_states_.end()) {
    log_error(
        "BlockIDStateManager: committing a block id {} that does not exist",
        block_id);
    return;
  }
  if (it->second != BlockIDState::kClosed) {
    log_error(
        "BlockIDStateManager: committing a block id {} that is not closed",
        block_id);
    return;
  }
  last_committed_block_id_ = block_id;
  for (auto it = block_id_states_.begin(); it != block_id_states_.end();) {
    if (it->first < *last_committed_block_id_) {
      it = block_id_states_.erase(it);
    }
    else {
      it++;
    }
  }
}

void
BlockIDStateManager::log_block_abandon(BlockID block_id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = block_id_states_.find(block_id);
  if (it != block_id_states_.end()) {
    it->second = BlockIDState::kAbandoned;
  }
}

} // namespace hogd
