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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "hogd/hog_db/background_commit_worker.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/status.h"
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

  RPCContext(HogDbDAO db, std::unique_ptr<sched_db_client_t> sched_db_client)
    : db_(std::move(db)), sched_db_client_(std::move(sched_db_client))
  {
    HogDbStatus<DbTip> ret = db_.read_dbtip();
    if (ret.has_value()) {
      // There is an existing dbtip, meaning that blocks have already been
      // committed to the db.
      *tip_.take() = *ret;
      current_committed_id_ = ret->id;
      current_committed_height_ = ret->value.proposal_header.block_height;
    }
    else if (ret.error().code() != HogDbCode::kNotFound) {
      ret.error().raise("failed to read DbTip");
    }
  }

  ~RPCContext();

  // getters.
  HogDbDAO &get_db() { return db_; }
  DbTip get_tip() { return *tip_.take(); }
  static bool has_committed_tip(DbTip const &tip)
  {
    return tip.value.proposal_header.block_height >= 0;
  }
  bool has_committed_tip() { return has_committed_tip(get_tip()); }
  void set_tip(DbTip new_tip)
  {
    const BlockTXID id = new_tip.id;
    const BlockHeight height = new_tip.value.proposal_header.block_height;
    auto tip = tip_.take();
    *tip = std::move(new_tip);
    current_committed_height_.store(height, std::memory_order_relaxed);
    current_committed_id_.store(id, std::memory_order_release);
  }
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

  // Install the chain-configured default wallet into the tip. Called at
  // genesis load, before the genesis commit; update_dbtip preserves the
  // field, so the commit's write_dbtip persists it.
  void set_default_wallet(DeployedContractHash const &default_wallet)
  {
    tip_.with([&](DbTip &tip) { tip.default_wallet = default_wallet; });
  }

  // This shard's identity, resolved at startup (see hog_db_init). Genesis uses
  // it to write only the items this shard owns while still deriving the global
  // block hash. Defaults to single-shard {0, 1} until set.
  void set_shard_identity(ShardIdentity identity)
  {
    shard_identity_ = identity;
  }
  ShardIdentity const &get_shard_identity() const { return shard_identity_; }

  // opens a new block transaction.
  // Does not acquire a reference to the blocktx.
  HogDbStatus<PerThreadARC<BlockTX> *> open_block_tx(BlockTXID bid,
                                                     BlockExecutionMode mode);

  // Opens a new block transaction only if parent_id is still the committed
  // tip. If a commit is currently being published, waits until the tip and
  // committed data are consistent before checking parent_id.
  HogDbStatus<PerThreadARC<BlockTX> *>
  open_block_tx_at_parent(BlockTXID parent_id, BlockTXID bid,
                          BlockExecutionMode mode);

  // Opens a viewing block transaction and returns the tip it views. If a commit
  // is currently being published, waits until the tip and committed data are
  // consistent before opening.
  HogDbStatus<ViewingSnapshot> open_viewing_snapshot(BlockTXID bid);

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

  HogDbStatus<void> update_dbtip(BlockTXID bid, ConsensusValue value,
                                 const StorageLongHash &db_hash);

  void cache_db_hash(BlockTXID bid, const StorageLongHash &db_hash)
  {
    std::scoped_lock g(lock_);
    db_hashes_[bid] = db_hash;
  }

  std::optional<StorageLongHash> take_db_hash(BlockTXID bid)
  {
    std::scoped_lock g(lock_);
    auto it = db_hashes_.find(bid);
    if (it == db_hashes_.end()) {
      return std::nullopt;
    }
    StorageLongHash out = it->second;
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
  // The commit_publishing_ load is sequenced before the tip loads on purpose:
  // the committer clears commit_publishing_ (release) only after
  // update_dbtip, so observing it false here guarantees the subsequent
  // loads reflect the post-commit tip. The ID's release/acquire pairing also
  // publishes the corresponding height before readers compare either field.
  bool is_committed_read_stale(DbTip const &snapshot)
  {
    if (commit_publishing_.load(std::memory_order_acquire)) {
      return true;
    }
    const BlockTXID committed_id =
        current_committed_id_.load(std::memory_order_acquire);
    const BlockHeight committed_height =
        current_committed_height_.load(std::memory_order_relaxed);
    return committed_id != snapshot.id ||
           committed_height != snapshot.value.proposal_header.block_height;
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
  HogDbDAO db_;
  x23::MVar<DbTip> tip_{[] {
    ConsensusValue value{};
    value.proposal_header.block_height = -1;
    return DbTip{.id = std::numeric_limits<BlockTXID>::max(),
                 .storage_long_hash = {},
                 .value = std::move(value)};
  }()};
  // Scalar mirror of tip_.id for the per-RPC snapshot staleness check. DbTip
  // contains the 2 KiB storage hash and a non-trivial ConsensusValue, so
  // copying it here would serialize hot executor traffic on tip_'s lock.
  // UINT64_MAX is already the persisted sentinel for "no committed tip".
  std::atomic<BlockTXID> current_committed_id_{
      std::numeric_limits<BlockTXID>::max()};
  std::atomic<BlockHeight> current_committed_height_{-1};
  // the storage cost model for computing storage costs at the current block
  // height.
  std::optional<StorageCostModel> cost_model_;
  // This shard's identity; see set_shard_identity. Single-shard until set.
  ShardIdentity shard_identity_{.shard_id = 0, .total_shards = 1};
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
  std::map<BlockTXID, StorageLongHash> db_hashes_;
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

  HogDbStatus<PerThreadARC<BlockTX> *>
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
  const BlockTXID committed_id =
      current_committed_id_.load(std::memory_order_acquire);
  if (committed_id != std::numeric_limits<BlockTXID>::max() &&
      committed_id == bid) {
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
