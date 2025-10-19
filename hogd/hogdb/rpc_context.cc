#include "hogd/hogdb/rpc_context.h"

#include <cassert>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<PerThreadARC<BlockTX> *>
RPCContext::open_block_tx(BlockTXID bid)
{
  // create the block tx.
  std::scoped_lock g(lock_);
  auto [it, inserted] = block_txes_.emplace(
      std::piecewise_construct, std::forward_as_tuple(bid),
      std::forward_as_tuple(get_db(), get_trie(), get_cost_model(),
                            sched_db_client_, get_tip().height));
  if (!inserted) {
    warn("Attempt to open block tx {} that already exists", bid);
    return HogDBError(HogDBCode::kExists);
  }

  return &it->second;
}

PerThreadARC<BlockTX> *
RPCContext::get_block_tx_slow(BlockTXID bid, bool acquire_reference)
{
  PerThreadARC<BlockTX> *tx;
  // This lock should be held during the cache updates, not just the lookup.
  // Otherwise, technically, release_block_tx_reference() could be called in
  // between when we get the tx pointer from block_txes_ and when we insert it
  // into the cache.
  std::scoped_lock g(lock_);

  // retrieve the block tx (if it exists).
  {
    auto it = block_txes_.find(bid);
    if (it == block_txes_.end()) {
      tx = nullptr;
    }
    else {
      tx = &(it->second);
    }
  }
  if (!tx)
    return nullptr;

  if (acquire_reference && !tx->try_acquire_reference()) [[unlikely]]
    return nullptr;

  // then make sure it's in the cache so we can avoid this cost next time.
  BlockCache &cache = *block_tx_cache_;
  std::scoped_lock g2(cache.lock);
  cache.blocks.insert(std::make_pair(bid, tx));
  return tx;
}

void
RPCContext::remove_shutdown_block_tx(BlockTXID bid)
{
  std::scoped_lock g(lock_);
  // remove each copy of the block tx that was cached.
  std::unique_lock ul = block_tx_cache_.lock_threads();
  block_tx_cache_.for_each(ul, [bid](BlockCache &cache) {
    std::scoped_lock g(cache.lock);
    cache.blocks.erase(bid);
  });

  // remove the block tx.
  //
  // We make sure to delete the BlockTX _after_
  // deleting the pointers to it in the threadlocal caches.
  block_txes_.erase(bid);
}

HogDBStatus<void>
RPCContext::update_dbtip(BlockTXID bid)
{
  trie_.generate_top_level_hashes();
  tip_.hash = trie_.get_root_hash();
  tip_.height = tip_.height + 1;
  tip_.id = bid;
  return get_db().write_dbtip(tip_);
}

RPCContext::~RPCContext()
{
  // This destructor only matters for debugging -- making sure that refcounts
  // are properly maintained. We could just silently drop both the InvokeTXs and
  // the references they have to the BlockTXs.

  invoke_txes_.for_each(
      [](auto &tx) { tx.second.release_blocktx_reference(); });
}

} // namespace hogd
