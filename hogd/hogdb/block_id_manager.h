#pragma once

#include <mutex>
#include <optional>
#include <unordered_map>

#include "rpc_types.hh"

namespace hogd {

// Debug-only structure to track state of different block ids
// (and log more helpful errors when debugging system integration)
class BlockIDStateManager {
public:
  // Either the genesis block, or the last committed block id.
  // Set when initializing from db.
  void log_block_id_tip(BlockID block_id);

  void log_block_open(BlockID parent_id, BlockID block_id);
  void log_block_close(BlockID block_id);
  void log_block_commit(BlockID block_id);
  void log_block_abandon(BlockID block_id);

  inline static bool throw_on_error_ = false;

private:
  enum class BlockIDState { kOpened, kClosed, kAbandoned };

  std::mutex mtx_;
  std::unordered_map<BlockID, BlockIDState> block_id_states_;
  // If there is a block id that committed, then we assume that all future block
  // ids will be strictly larger than the last committed one.
  std::optional<BlockID> last_committed_block_id_;
};

class UncheckedBlockIDStateManager {
public:
  void log_block_id_tip(BlockID /*unused*/) {}
  void log_block_open(BlockID /*unused*/, BlockID /*unused*/) {}
  void log_block_close(BlockID /*unused*/) {}
  void log_block_commit(BlockID /*unused*/) {}
  void log_block_abandon(BlockID /*unused*/) {}
};

} // namespace hogd
