// rpc_context.h - maintains global state shared across all RPCs
//
// TODO(abelay): if we had RCU, the retrieval of BlockTX, which is read mostly,
// would be a lot easier.
//
// TODO(abelay): need to think harder about the reference counting strategy for
// BlockTX.

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/status.h"

namespace hogd {

// RPCContext contains all the state that needs to be shared across RPCs handled
// by the database.
class RPCContext {
public:
  RPCContext() = default;

  // getters & setters.
  HogDatabase &get_db() const { return *db_; }
  void set_db(HogDatabase *db) { db_ = db; }

  // retrieves a block transaction (and creates it if it doesn't exist).
  BlockTX &get_block_tx(BlockTXID bid);
  // removes a block transaction.
  void close_block_tx(BlockTXID bid);

  // retrieves an invoke transaction (and creates it if it doesn't
  // exist).
  InvokeTX &get_invoke_tx(BlockTXID bid, InvokeTXID iid);
  // removes an invoke transaction.
  void close_invoke_tx(BlockTXID bid, InvokeTXID iid);

private:
  struct BlockCache {
    std::mutex lock;
    std::unordered_map<BlockTXID, BlockTX *> blocks;

    BlockCache() = default;
    BlockCache(BlockCache &&cache) noexcept : blocks(std::move(cache.blocks)) {}
  };

  // the database used to persist state.
  HogDatabase *db_{nullptr};
  // pending invocation transactions.
  SynchronizedMap<std::pair<BlockTXID, InvokeTXID>, InvokeTX> invoke_txes_;
  // protects block_txes_.
  std::mutex lock_;
  // pending block transactions.
  std::unordered_map<BlockTXID, BlockTX> block_txes_;
  // used to reduce contention in get_block_tx(), which is called often.
  PerThread<BlockCache> block_tx_cache_;

  // slow path for get_block_tx().
  BlockTX &get_block_tx_slow(BlockTXID bid);
};

inline BlockTX &
RPCContext::get_block_tx(BlockTXID bid)
{
  BlockCache &cache = *block_tx_cache_;
  std::scoped_lock g(cache.lock); // NOTE: very low contention
  auto it = cache.blocks.find(bid);
  if (it == cache.blocks.end())
    return get_block_tx_slow(bid);
  return *it->second;
}

inline InvokeTX &
RPCContext::get_invoke_tx(BlockTXID bid, InvokeTXID iid)
{
  auto key = std::make_pair(bid, iid);
  auto [tx, ul] = invoke_txes_.find(key);
  if (!tx) {
    tx = invoke_txes_.insert(key, InvokeTX(get_block_tx(bid)), ul);
    assert(tx != nullptr);
  }
  return *tx;
}

inline void
RPCContext::close_invoke_tx(BlockTXID bid, InvokeTXID iid)
{
  invoke_txes_.remove(std::make_pair(bid, iid));
}

// myctx returns the RPC context for the process. This is used to access
// important state that is shared across all RPCs.
[[nodiscard, gnu::pure]] inline RPCContext &
myctx()
{
  static RPCContext ctx;
  return ctx;
}

} // namespace hogd
