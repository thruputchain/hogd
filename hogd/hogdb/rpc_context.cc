#include "hogd/hogdb/rpc_context.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/status.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

HogDBStatus<PerThreadARC<BlockTX> *>
RPCContext::open_block_tx(BlockTXID bid, BlockExecutionMode mode)
{
  std::scoped_lock g(lock_);
  return open_block_tx_locked(bid, mode);
}

HogDBStatus<PerThreadARC<BlockTX> *>
RPCContext::open_block_tx_locked(BlockTXID bid, BlockExecutionMode mode)
{
  // create the block tx.
  auto [it, inserted] = block_txes_.emplace(
      std::piecewise_construct, std::forward_as_tuple(bid),
      std::forward_as_tuple(get_db(), get_cost_model(),
                            background_commit_worker_,
                            get_tip().value.proposal_header.block_height + 1,
                            mode));
  if (!inserted) {
    warn("Attempt to open block tx {} that already exists", bid);
    return HogDBError(HogDBCode::kExists);
  }

  return &it->second;
}

HogDBStatus<PerThreadARC<BlockTX> *>
RPCContext::open_block_tx_at_parent(BlockTXID parent_id, BlockTXID bid,
                                    BlockExecutionMode mode)
{
  while (true) {
    {
      std::scoped_lock g(lock_);
      if (!commit_publishing_.load(std::memory_order_acquire)) {
        if (parent_id != get_tip().id) {
          return HogDBError(HogDBCode::kStaleBlock);
        }
        return open_block_tx_locked(bid, mode);
      }
    }
    commit_publishing_.wait(true, std::memory_order_acquire);
  }
}

HogDBStatus<ViewingSnapshot>
RPCContext::open_viewing_snapshot(BlockTXID bid)
{
  while (true) {
    {
      std::scoped_lock g(lock_);
      if (!commit_publishing_.load(std::memory_order_acquire)) {
        DbTip tip = get_tip();
        if (tip.value.proposal_header.block_height < 0) {
          return HogDBError(HogDBCode::kNotFound);
        }
        auto res = open_block_tx_locked(
            bid, BlockExecutionMode::kBlockExecutionModeViewing);
        if (!res) {
          return res.error();
        }

        ViewingSnapshot snapshot;
        snapshot.viewing_block_id = bid;
        snapshot.tip = std::move(tip);
        return snapshot;
      }
    }
    commit_publishing_.wait(true, std::memory_order_acquire);
  }
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

bool
RPCContext::is_snapshot_read_stale_slow(BlockTXID bid)
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
      return true;
    }
    tx = &(it->second);
  }
  assert(tx);

  // then make sure it's in the cache so we can avoid this cost next time.
  BlockCache &cache = *block_tx_cache_;
  std::scoped_lock g2(cache.lock);
  cache.blocks.insert(std::make_pair(bid, tx));
  return tx->get().is_invalidated();
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

  // Drop any closed-but-uncommitted db hash cached for this block. This is the
  // sole site that removes a BlockTX, so clearing here guarantees the cached
  // hash never outlives its block. The commit path takes (erases) the hash via
  // take_db_hash() before reaching teardown, so this erase is a no-op there;
  // it matters on the abandon / deferred-GC paths, where the hash would
  // otherwise leak until process exit.
  db_hashes_.erase(bid);
}

void
RPCContext::invalidate_siblings(BlockTXID committer)
{
  std::scoped_lock g(lock_);
  invalidate_siblings_locked(committer);
}

void
RPCContext::invalidate_siblings_locked(BlockTXID committer)
{
  for (auto &[id, arc] : block_txes_) {
    if (id != committer) {
      arc.get().invalidate();
    }
  }
}

void
RPCContext::begin_commit_publication(BlockTXID committer)
{
  std::scoped_lock g(lock_);
  CHECK(!commit_publishing_.load(std::memory_order_relaxed));
  commit_publishing_.store(true, std::memory_order_release);
  invalidate_siblings_locked(committer);
}

void
RPCContext::finish_commit_publication() noexcept
{
  commit_publishing_.store(false, std::memory_order_release);
  commit_publishing_.notify_all();
}

RPCContext::CommitPublicationGuard::CommitPublicationGuard(RPCContext &ctx,
                                                           BlockTXID committer)
  : ctx_(&ctx)
{
  ctx_->begin_commit_publication(committer);
}

RPCContext::CommitPublicationGuard::~CommitPublicationGuard()
{
  ctx_->finish_commit_publication();
}

HogDBStatus<void>
RPCContext::update_dbtip(BlockTXID bid, ConsensusValue value,
                         const StorageLongHash &db_hash)
{
  DbTip new_tip;
  tip_.with([&](DbTip &tip) {
    tip.value = std::move(value);
    tip.storage_long_hash = db_hash;
    tip.id = bid;
    current_committed_id_.store(bid, std::memory_order_release);
    new_tip = tip;
  });
  current_committed_height_.store(new_tip.value.proposal_header.block_height);
  return get_db().write_dbtip(new_tip);
}

// Members are destroyed in reverse declaration order: invoke_txes_ goes
// first, so each leftover InvokeTX releases its BlockTX reference (possibly
// garbage collecting an abandoned block through remove_shutdown_block_tx(),
// whose state is declared before invoke_txes_ and is therefore still alive),
// and only then are the remaining BlockTXs destroyed.
RPCContext::~RPCContext() = default;

} // namespace hogd
