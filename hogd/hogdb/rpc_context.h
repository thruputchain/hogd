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
// Each InvokeTX owns a counted reference to its PerThreadARC<BlockTX>;
// destroying the InvokeTX releases the reference, only after destructor-time
// cleanup, since that cleanup dereferences the block (see BlockTXRef, #2959).
// The PerThreadARC<BlockTX> lives until both there are no references
// remaining to it, and the PerThreadARC has been explicitly shutdown (via
// initiate_shutdown()). Closing the BlockTX initiates shutdown of the
// PerThreadARC<BlockTX>. If the BlockTX is shutdown and the reference
// released is the last reference, the BlockTX is garbage collected (that
// condition is checked when an InvokeTX is destroyed, and when the BlockTX is
// committed or abandoned).  When a BlockTX is committed, all pending
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
  static bool has_committed_tip(DbTip const &tip) { return tip.height >= 0; }
  bool has_committed_tip() { return has_committed_tip(get_tip()); }
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

  // Opens a new block transaction only if parent_id is still the committed
  // tip. If a commit is currently being published, waits until the tip and
  // committed data are consistent before checking parent_id.
  HogDBStatus<PerThreadARC<BlockTX> *>
  open_block_tx_at_parent(BlockTXID parent_id, BlockTXID bid,
                          BlockExecutionMode mode);

  // Opens a viewing block transaction and returns the tip it views. If a commit
  // is currently being published, waits until the tip and committed data are
  // consistent before opening.
  HogDBStatus<ViewingSnapshot> open_viewing_snapshot(BlockTXID bid);

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

  const auto &get_sched_db_client() { return sched_db_client_; }

  // The block ID must have already been shut down and have no outstanding
  // references.
  void remove_shutdown_block_tx(BlockTXID bid);

  void invalidate_siblings(BlockTXID committer);

  bool is_snapshot_read_stale(BlockTXID bid);

  // Staleness check for reads served directly against the committed tip
  // (no BlockTX snapshot), e.g. the public db gateway. A block commit
  // applies its data to the live DAO (BlockTX::commit) *before* it advances
  // the tip (update_dbtip), and holds commit_publishing_ across that whole
  // window (see commit_block / CommitPublicationGuard). So a committed-tip
  // read can observe height-(H+1) data while the tip still reads H.
  //
  // Callers snapshot the tip, perform their reads, then call this with the
  // snapshot: a true result means a commit landed (or is mid-publication)
  // during the read, so the snapshot's height label can no longer be
  // trusted. Mirrors is_snapshot_read_stale() for the no-BlockTX path.
  //
  // The commit_publishing_ load is sequenced before get_tip() on purpose:
  // the committer clears commit_publishing_ (release) only after
  // update_dbtip, so observing it false here guarantees the subsequent
  // get_tip() reflects the post-commit tip.
  bool is_committed_read_stale(DbTip const &snapshot)
  {
    if (commit_publishing_.load(std::memory_order_acquire)) {
      return true;
    }
    DbTip tip = get_tip();
    return tip.id != snapshot.id || tip.height != snapshot.height;
  }

  // Block until no commit is mid-publication, so a committed-tip reader can
  // start from a point where the tip and ramdb data are consistent. Returns
  // immediately if nothing is publishing. Used to keep committed-tip read
  // retries from spinning through a publication window.
  void wait_for_no_commit_publishing()
  {
    commit_publishing_.wait(true, std::memory_order_acquire);
  }

  class CommitPublicationGuard {
  public:
    CommitPublicationGuard(RPCContext &ctx, BlockTXID committer);
    CommitPublicationGuard(CommitPublicationGuard const &) = delete;
    CommitPublicationGuard &operator=(CommitPublicationGuard const &) = delete;
    CommitPublicationGuard(CommitPublicationGuard &&) = delete;
    CommitPublicationGuard &operator=(CommitPublicationGuard &&) = delete;
    ~CommitPublicationGuard();

  private:
    RPCContext *ctx_;
  };

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
  std::atomic<bool> commit_publishing_{false};
  // pending block transactions.
  // The block txs are refcounted.
  // Each InvokeTX acquires a reference;  when the InvokeTX is closed, it
  // releases the reference, and garbage collects the block tx if the blocktx is
  // shutdown and the reference closed is the last reference.
  std::unordered_map<BlockTXID, PerThreadARC<BlockTX>> block_txes_;
  // used to reduce contention in get_block_tx(), which is called often.
  PerThread<BlockCache> block_tx_cache_;
  // closed snapshot hashes waiting for commit.
  // Declared (with lock_, block_txes_, and block_tx_cache_) before
  // invoke_txes_: destroying an InvokeTX can garbage collect an abandoned
  // block through remove_shutdown_block_tx(), which touches all of them, so
  // they must outlive invoke_txes_ during ~RPCContext().
  std::map<BlockTXID, DbHash> db_hashes_;
  // pending invocation transactions.
  SynchronizedMap<InvokeTxKey, InvokeTX, InvokeTxKey::Hash> invoke_txes_;

  // External services that may subscribe to changes in the database.
  std::shared_ptr<sched_db_client_t> sched_db_client_;
  AuthChangeSubscriptionManager auth_change_subscription_manager_;
  BackgroundCommitWorker background_commit_worker_ =
      BackgroundCommitWorker(auth_change_subscription_manager_,
                             sched_db_client_);

  // slow path for get_block_tx().
  PerThreadARC<BlockTX> *get_block_tx_slow(BlockTXID bid,
                                           bool acquire_reference);
  // slow path for is_snapshot_read_stale().
  bool is_snapshot_read_stale_slow(BlockTXID bid);

  HogDBStatus<PerThreadARC<BlockTX> *>
  open_block_tx_locked(BlockTXID bid, BlockExecutionMode mode);

  void invalidate_siblings_locked(BlockTXID committer);
  void begin_commit_publication(BlockTXID committer);
  void finish_commit_publication() noexcept;

  void release_block_tx_reference(BlockTXID bid);
};

// Returns true if the snapshot id is neither the committed tip nor a live
// non-abandoned blocktx.
// Safe without acquiring a reference: the per-thread cache.lock (fast path) and
// lock_ (slow path) prevent concurrent removal by remove_shutdown_block_tx().
inline bool
RPCContext::is_snapshot_read_stale(BlockTXID bid)
{
  DbTip tip = get_tip();
  if (has_committed_tip(tip) && tip.id == bid) {
    return false;
  }

  {
    BlockCache &cache = *block_tx_cache_;
    std::scoped_lock g(cache.lock);
    auto it = cache.blocks.find(bid);
    if (it != cache.blocks.end()) {
      return it->second->get().is_invalidated();
    }
  }

  return is_snapshot_read_stale_slow(bid);
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
    // get_block_tx() acquires a reference to the blocktx; the InvokeTX owns
    // it from here on.
    tx = invoke_txes_.insert(key, InvokeTX(*btx, *this, bid), ul);
    assert(tx != nullptr);
  }
  return tx;
}

inline void
RPCContext::close_invoke_tx(BlockTXID bid, InvokeTXID iid, InvokeTX *itx)
{
  // Steal [itx]'s contents into a local so that its destructor runs after
  // the map entry is erased and the bucket lock dropped: destroying the
  // InvokeTX releases its counted BlockTX reference -- only after
  // destructor-time cleanup -- and garbage collects a shutdown block on the
  // last release (see BlockTXRef, #2959), which is too much work to hold the
  // bucket lock across. Nothing may release the reference before the
  // InvokeTX's cleanup has finished: another close could then free the
  // BlockTX while this one's destructor still dereferences it.
  InvokeTX closing(std::move(*itx));
  invoke_txes_.remove(InvokeTxKey{.block_tx_id = bid, .invoke_tx_id = iid});
}

} // namespace hogd
