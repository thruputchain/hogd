#include "hogd/hogexec/interface/rpc_context.h"

#include <memory>
#include <mutex>
#include <utility>

#include "hogd/hogexec/interface/executor_context.h"
#include "rpc_types.hh"

namespace hogd {

std::shared_ptr<ExecutorContext> HogexecRpcContext::get_executor_context_slow(BlockID block_id)
{
    auto r =  executor_state_manager_.get_executor_context(block_id);
    if (r) {
        BlockCache &cache = *executor_context_cache_;
        std::scoped_lock g(cache.lock);
        cache.blocks.insert(std::make_pair(block_id, r));
        return r;
    }
    return nullptr;
}

void HogexecRpcContext::open_proposal_block(BlockID block_id)
{
    executor_state_manager_.open_proposal(block_id);
}

void HogexecRpcContext::open_validation_block(BlockID block_id)
{
    executor_state_manager_.open_validation(block_id);
}

void HogexecRpcContext::close_block(BlockID block_id)
{
    executor_state_manager_.remove_block(block_id);

    std::unique_lock ul = executor_context_cache_.lock_threads();
    executor_context_cache_.for_each(ul, [block_id](BlockCache &cache) {
      std::scoped_lock g(cache.lock);
      cache.blocks.erase(block_id);
    });
}


} // namespace hogd