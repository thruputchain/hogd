#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

class EventRPCContext {
public:
  EventRPCContext(std::string_view work_directory)
    : work_directory_(work_directory)
  {}

  // Acquires a reference to the PerThreadARC<EventBlockContext> for the given
  // block id.
  // Returns nullptr if the block is not open.
  PerThreadARC<EventBlockContext> *get_context(BlockID block_id);

  // Opens a new block.
  // Returns an error if the block already exists.
  EventStatus<void> open_block(BlockID block_id);

  // Closes a block.
  // Returns the root hash of the block if successful.
  // Returns an error if the block is not open.
  EventStatus<SHA256Hash> close_block(BlockID block_id);

  // Abandons a block.
  // Returns an error if the block is not open.
  EventStatus<void> abandon_block(BlockID block_id);

  // This is normally called by commit_block() or abandon_block().
  // However, if the block is abandoned, it might be called by some caller
  // that called get_context(), if the block is abandoned while an outstanding
  // rpc from the executor is in flight.
  void gc_context(BlockID block_id);

private:
  PerThreadARC<EventBlockContext> *get_context_slow(BlockID block_id);
  void gc_context_internal(
      std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator it,
      std::unique_lock<std::mutex> &ul);

  struct ContextCache {
    ContextCache() = default;
    ContextCache(ContextCache &&cache) noexcept
      : contexts_(std::move(cache.contexts_))
    {}
    std::mutex mtx_;
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext> *> contexts_;
  };

  std::mutex mtx_;
  std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>
      event_block_contexts_;

  PerThread<ContextCache> cache_;
  std::string work_directory_;
};

} // namespace hogd
