// rpc_context.h - maintains global state shared across all RPCs
//
// TODO(abelay): if we had RCU, the retrieval of BlockTX, which is read mostly,
// would be a lot easier.
//
// TODO(abelay): need to think harder about the reference counting strategy for
// BlockTX.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "types.hh"
#include "x23/mvar.h"
#include "x23/rpc.h"

namespace hogd {

// RPCContext contains all the state that needs to be shared across RPCs handled
// by the database.
//
// The block txs are refcounted.
// Each InvokeTX acquires a reference; when the invoke tx is closed, it releases
// the reference to the PerThreadARC<BlockTX>. The PerThreadARC<BlockTX> lives
// until both there are no references remaining to it, and the PerThreadARC has
// been explicitly shutdown (via initiate_shutdown()). Closing the BlockTX
// initiates shutdown of the PerThreadARC<BlockTX>. If the BlockTX is shutdown
// and the reference closed is the last reference, the BlockTX is garbage
// collected (that condition is checked when an InvokeTX is closed, and when the
// BlockTX is committed or abandoned).  When a BlockTX is committed, all pending
// InvokeTXs must have been closed (so the refcount must be zero). If the
// blockTX is abandoned, it is allowed for there to be pending, outstanding
// InvokeTXs. However, you should eventually close all pending InvokeTXs, or
// else the BlockTX and InvokeTXs will never be garbage collected.
class RPCContext {
public:
  using sched_db_client_t =
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  RPCContext(HogDBDAO db, std::unique_ptr<sched_db_client_t> sched_db_client)
    : db_(std::move(db)), sched_db_client_(std::move(sched_db_client))
  {
    HogDBStatus<DbTip> ret = db_.read_dbtip();
    if (ret.has_value()) {
      // There is an existing dbtip, meaning that blocks have already been
      // committed to the db. If [ret.has_value()] is false, then no blocks have
      // been committed yet.
      *tip_.take() = *ret;
      current_committed_height_ = ret->height;
    }
  }

  ~RPCContext();

  // getters.
  HogDBDAO &get_db() { return db_; }
  DbTip get_tip() { return *tip_.take(); }
  void set_tip(DbTip new_tip) { *tip_.take() = std::move(new_tip); }
  AuthChangeSubscriptionManager &get_auth_change_subscription_manager()
  {
    return auth_change_subscription_manager_;
  }

  // Get the committed block height, when exact synchronization with DbTip is
  // not required. current_committed_height_ is updated when a block is
  // committed, but might be slightly stale.
  BlockHeight get_height_threadsafe()
  {
    return current_committed_height_.load();
  }

  StorageCostModel const &get_cost_model()
  {
    assert(cost_model_.has_value());
    return *cost_model_;
  }

  void initialize_cost_model(StorageCostModel cost_model)
  {
    cost_model_.emplace(cost_model);
  }

  // opens a new block transaction.
  // Does not acquire a reference to the blocktx.
  HogDBStatus<PerThreadARC<BlockTX> *> open_block_tx(BlockTXID bid,
                                                     BlockExecutionMode mode);
  // retrieves an open block transaction.
  // If the returned value is not null, then
  // the caller has acquired a reference to the blocktx.
  PerThreadARC<BlockTX> *get_block_tx(BlockTXID bid,
                                      bool acquire_reference = true);

  // retrieves an invoke transaction (and creates it if it doesn't exist).
  // Returns nullptr if the bid is not open.
  InvokeTX *get_invoke_tx(BlockTXID bid, InvokeTXID iid);
  // removes an invoke transaction.
  // If the blocktx is shutdown and the reference closed is the last reference,
  // the blocktx is garbage collected.
  void close_invoke_tx(BlockTXID bid, InvokeTXID iid, InvokeTX *itx);

  HogDBStatus<void> update_dbtip(BlockTXID bid, BlockHash block_hash,
                                 SHA256Hash events_hash, const DbHash &db_hash);

  void cache_db_hash(BlockTXID bid, const DbHash &db_hash)
  {
    std::scoped_lock g(lock_);
    db_hashes_[bid] = db_hash;
  }

  std::optional<DbHash> take_db_hash(BlockTXID bid)
  {
    std::scoped_lock g(lock_);
    auto it = db_hashes_.find(bid);
    if (it == db_hashes_.end()) {
      return std::nullopt;
    }
    DbHash out = it->second;
    db_hashes_.erase(it);
    return out;
  }

  void clear_db_hash(BlockTXID bid)
  {
    std::scoped_lock g(lock_);
    db_hashes_.erase(bid);
  }

  const auto &get_sched_db_client() { return sched_db_client_; }

  // The block ID must have already been shut down and have no outstanding
  // references.
  void remove_shutdown_block_tx(BlockTXID bid);

  void invalidate_siblings(BlockTXID committer);

  bool is_block_tx_stale(BlockTXID bid);

private:
  struct BlockCache {
    std::mutex lock;
    std::unordered_map<BlockTXID, PerThreadARC<BlockTX> *> blocks;

    BlockCache() = default;
    BlockCache(BlockCache &&cache) noexcept : blocks(std::move(cache.blocks)) {}
  };

  // the database used to persist state.
  HogDBDAO db_;
  x23::MVar<DbTip> tip_{DbTip{.height = -1, .id = UINT64_MAX}};
  std::atomic<BlockHeight> current_committed_height_{-1};
  // the storage cost model for computing storage costs at the current block
  // height.
  std::optional<StorageCostModel> cost_model_;
  // protects block_txes_.
  std::mutex lock_;
  // pending block transactions.
  // The block txs are refcounted.
  // Each InvokeTX acquires a reference;  when the InvokeTX is closed, it
  // releases the reference, and garbage collects the block tx if the blocktx is
  // shutdown and the reference closed is the last reference.
  std::unordered_map<BlockTXID, PerThreadARC<BlockTX>> block_txes_;
  // used to reduce contention in get_block_tx(), which is called often.
  PerThread<BlockCache> block_tx_cache_;
  // pending invocation transactions.
  SynchronizedMap<InvokeTxKey, InvokeTX, InvokeTxKey::Hash> invoke_txes_;
  // closed snapshot hashes waiting for commit.
  std::map<BlockTXID, DbHash> db_hashes_;

  // External serivces that may subscribe to changes in the database.
  std::shared_ptr<sched_db_client_t> sched_db_client_;
  AuthChangeSubscriptionManager auth_change_subscription_manager_;
  BackgroundCommitWorker background_commit_worker_ =
      BackgroundCommitWorker(auth_change_subscription_manager_,
                             sched_db_client_);

  // slow path for get_block_tx().
  PerThreadARC<BlockTX> *get_block_tx_slow(BlockTXID bid,
                                           bool acquire_reference);
  // slow path for is_block_tx_stale().
  bool is_block_tx_stale_slow(BlockTXID bid);

  void release_block_tx_reference(BlockTXID bid);
};

// Returns true if the blocktx does not exist or is on an abandoned branch.
// Safe without acquiring a reference: the per-thread cache.lock (fast path) and
// lock_ (slow path) prevent concurrent removal by remove_shutdown_block_tx().
inline bool
RPCContext::is_block_tx_stale(BlockTXID bid)
{
  {
    BlockCache &cache = *block_tx_cache_;
    std::scoped_lock g(cache.lock);
    auto it = cache.blocks.find(bid);
    if (it != cache.blocks.end()) {
      return it->second->get().is_invalidated();
    }
  }

  return is_block_tx_stale_slow(bid);
}

inline PerThreadARC<BlockTX> *
RPCContext::get_block_tx(BlockTXID bid, bool acquire_reference)
{
  {
    BlockCache &cache = *block_tx_cache_;
    std::scoped_lock g(cache.lock); // NOTE: very low contention
    auto it = cache.blocks.find(bid);
    if (it != cache.blocks.end()) {
      if (acquire_reference && !it->second->try_acquire_reference())
          [[unlikely]]
        return nullptr;
      return (it->second);
    }
  }

  return get_block_tx_slow(bid, acquire_reference);
}

inline InvokeTX *
RPCContext::get_invoke_tx(BlockTXID bid, InvokeTXID iid)
{
  InvokeTxKey key{.block_tx_id = bid, .invoke_tx_id = iid};
  auto [tx, ul] = invoke_txes_.find(key);
  if (!tx) {
    PerThreadARC<BlockTX> *btx = get_block_tx(bid);
    if (!btx) [[unlikely]]
      return nullptr;
    // get_block_tx() acquires a reference to the blocktx.
    tx = invoke_txes_.insert(key, InvokeTX(*btx), ul);
    assert(tx != nullptr);
  }
  return tx;
}

inline void
RPCContext::close_invoke_tx(BlockTXID bid, InvokeTXID iid, InvokeTX *itx)
{
  bool destroy_blocktx = itx->release_blocktx_reference();
  invoke_txes_.remove(InvokeTxKey{.block_tx_id = bid, .invoke_tx_id = iid});
  if (destroy_blocktx) {
    remove_shutdown_block_tx(bid);
  }
}

} // namespace hogd
