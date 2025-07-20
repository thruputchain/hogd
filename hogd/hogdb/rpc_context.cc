#include "hogd/hogdb/rpc_context.h"

#include <mutex>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"

namespace hogd {

BlockTX &
RPCContext::get_block_tx_slow(BlockTXID bid)
{
  BlockTX *tx;

  // retrieve or create the block tx.
  {
    std::scoped_lock g(lock_);
    auto [it, inserted] = block_txes_.emplace(bid, get_db());
    tx = &(it->second);
  }

  // then make sure it's in the cache so we can avoid this cost next time.
  BlockCache &cache = *block_tx_cache_;
  std::scoped_lock g(cache.lock);
  cache.blocks.insert(std::make_pair(bid, tx));
  return *tx;
}

void
RPCContext::close_block_tx(BlockTXID bid)
{
  // remove the block tx.
  std::scoped_lock g(lock_);
  block_txes_.erase(bid);

  // remove each copy of the block tx that was cached.
  std::unique_lock ul = block_tx_cache_.lock_threads();
  block_tx_cache_.for_each(ul, [bid](BlockCache &cache) {
    std::scoped_lock g(cache.lock);
    cache.blocks.erase(bid);
  });
}

} // namespace hogd
