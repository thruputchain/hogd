#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"

namespace hogd {

// Copying the pattern in hogdb/rpc_context.h
class HogexecRpcContext {
public:
  HogexecRpcContext(std::unique_ptr<DatabaseProxy> db_proxy,
                    WorkerPool &main_worker_pool)
    : executor_state_manager_(std::move(db_proxy), main_worker_pool)
  {}

  // Disable copy and move
  HogexecRpcContext(HogexecRpcContext const &) = delete;
  HogexecRpcContext &operator=(HogexecRpcContext const &) = delete;
  HogexecRpcContext(HogexecRpcContext &&) = delete;
  HogexecRpcContext &operator=(HogexecRpcContext &&) = delete;

  // It would be nice to not have to acquire a ref each time;
  // but I(geoff) don't know how to do that without worrying about a potential
  // race on shutdown/block closing.
  std::shared_ptr<ExecutorContext> get_executor_context(BlockID block_id);

  void open_proposal_block(BlockID block_id);
  void open_validation_block(BlockID block_id);
  void close_block(BlockID block_id);

private:
  struct BlockCache {
    std::mutex lock;
    std::unordered_map<BlockID, std::shared_ptr<ExecutorContext>> blocks;

    BlockCache() = default;
    BlockCache(BlockCache &&cache) noexcept : blocks(std::move(cache.blocks)) {}
  };

  std::shared_ptr<ExecutorContext> get_executor_context_slow(BlockID block_id);

  // Cache executor context locally
  PerThread<BlockCache> executor_context_cache_;

  ExecutorStateManager executor_state_manager_;
};

inline std::shared_ptr<ExecutorContext>
HogexecRpcContext::get_executor_context(BlockID block_id)
{
  {
    BlockCache &cache = *executor_context_cache_;
    std::scoped_lock g(cache.lock);
    auto it = cache.blocks.find(block_id);
    if (it != cache.blocks.end()) {
      return it->second;
    }
  }
  return get_executor_context_slow(block_id);
}

} // namespace hogd
