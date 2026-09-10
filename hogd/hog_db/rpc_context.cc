#include "hogd/hog_db/rpc_context.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/status.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

HogDbStatus<PerThreadARC<BlockTX> *>
RPCContext::open_block_tx(BlockTXID bid, BlockExecutionMode mode)
{
  std::scoped_lock g(lock_);
  return open_block_tx_locked(bid, mode);
}

HogDbStatus<PerThreadARC<BlockTX> *>
RPCContext::open_block_tx_locked(BlockTXID bid, BlockExecutionMode mode)
{
  // The new block's height is the tip's height + 1. A recovered tip can carry
  // a tampered maximum BlockHeight -- the <DBTIP> row is global metadata,
  // excluded from the state hash, so it survives recovery unchanged -- and
  // BlockHeight is signed, so evaluating that + 1 would be UB. No successor
  // block exists at the maximum height; reject it before constructing a
  // BlockTX (issue #3955).
  const BlockHeight parent_height =
      get_tip().value.proposal_header.block_height;
  if (parent_height == std::numeric_limits<BlockHeight>::max()) [[unlikely]] {
    error("Refusing to open block {}: tip height {} is the maximum "
          "representable BlockHeight, so it has no successor",
          bid, parent_height);
    return HogDbError(HogDbCode::kOverflow);
  }

  // create the block tx.
  auto [it, inserted] =
      block_txes_.emplace(std::piecewise_construct, std::forward_as_tuple(bid),
                          std::forward_as_tuple(get_db(), get_cost_model(),
                                                background_commit_worker_,
                                                parent_height + 1, mode));
  if (!inserted) {
    warn("Attempt to open block tx {} that already exists", bid);
    return HogDbError(HogDbCode::kExists);
  }

  return &it->second;
}

x23::Async<HogDbStatus<PerThreadARC<BlockTX> *>>
RPCContext::open_block_tx_at_parent(BlockTXID parent_id, BlockTXID bid,
                                    BlockExecutionMode mode)
{
  while (true) {
    {
      std::scoped_lock g(lock_);
      if (!commit_publishing_.load(std::memory_order_acquire)) {
        if (parent_id != get_tip().id) {
          co_return HogDbError(HogDbCode::kStaleBlock);
        }
        co_return open_block_tx_locked(bid, mode);
      }
    }
    co_await co_wait_for_no_commit_publishing();
  }
}

x23::Async<HogDbStatus<ViewingSnapshot>>
RPCContext::open_viewing_snapshot(BlockTXID bid)
{
  while (true) {
    {
      std::scoped_lock g(lock_);
      if (!commit_publishing_.load(std::memory_order_acquire)) {
        DbTip tip = get_tip();
        if (tip.value.proposal_header.block_height < 0) {
          co_return HogDbError(HogDbCode::kNotFound);
        }
        auto res = open_block_tx_locked(
            bid, BlockExecutionMode::kBlockExecutionModeViewing);
        if (!res) {
          co_return res.error();
        }

        ViewingSnapshot snapshot;
        snapshot.viewing_block_id = bid;
        snapshot.tip = std::move(tip);
        co_return snapshot;
      }
    }
    co_await co_wait_for_no_commit_publishing();
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
  // After the clear, so a woken coroutine's re-check observes it. The pool is
  // named explicitly: the committer usually runs on a worker of that very
  // pool, but tests finish publications from plain threads, where the
  // thread-local pool lookup broadcast() defaults to has nothing.
  publication_generation_.rmw([](auto &v) { ++v; });
  publication_generation_.broadcast(&default_threadpool());
}

x23::Async<void>
RPCContext::co_wait_for_no_commit_publishing()
{
  for (;;) {
    // The generation is read before the flag: a publication finishing
    // between the two bumps the generation, so the wait below returns
    // immediately rather than missing the wake.
    const auto generation = publication_generation_.load();
    if (!commit_publishing_.load(std::memory_order_acquire)) {
      co_return;
    }
    co_await publication_generation_.wait(generation);
  }
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

HogDbStatus<void>
RPCContext::update_dbtip(BlockTXID bid, ConsensusValue value,
                         const StorageLongHash &db_hash)
{
  DbTip new_tip;
  bool network_hash_mismatch = false;
  bool timepoint_went_backwards = false;
  tip_.with([&](DbTip &tip) {
    // The network hash is fixed for the life of the chain, but it lives in
    // the consensus value, which this replaces wholesale rather than carrying
    // forward the way default_wallet and network_config are. A caller that
    // builds a value without it moves the chain onto network zero, and
    // contracts reading GetNetworkHash then see a different network than
    // signatures are verified against. Genesis installs it -- the tip before
    // it is pre-genesis, at height -1 -- and every commit after must agree.
    if (tip.value.proposal_header.block_height >= 0 &&
        value.proposal_header.network_hash !=
            tip.value.proposal_header.network_hash) {
      network_hash_mismatch = true;
      return;
    }
    // Blocks only move forward in time. The timepoint reaches the db the same
    // way the network hash does -- carried on a consensus value that replaces
    // the tip wholesale -- so the same class of caller bug puts an older
    // timepoint on a newer block, and anything reading the chain back then
    // sees time run backwards across a commit. Equal is left alone: two
    // blocks sharing a nanosecond is implausible, not incoherent.
    if (tip.value.proposal_header.block_height >= 0 &&
        value.proposal_header.block_timepoint <
            tip.value.proposal_header.block_timepoint) {
      timepoint_went_backwards = true;
      return;
    }
    tip.value = std::move(value);
    tip.storage_long_hash = db_hash;
    tip.id = bid;
    current_committed_height_.store(tip.value.proposal_header.block_height,
                                    std::memory_order_relaxed);
    current_committed_id_.store(bid, std::memory_order_release);
    new_tip = tip;
  });
  if (network_hash_mismatch) [[unlikely]] {
    error("Refusing to commit block {}: its consensus value carries a "
          "different network hash than the committed tip",
          bid);
    return HogDbError(HogDbCode::kInternal);
  }
  if (timepoint_went_backwards) [[unlikely]] {
    error("Refusing to commit block {}: its consensus value carries a "
          "timepoint earlier than the committed tip's",
          bid);
    return HogDbError(HogDbCode::kInternal);
  }
  return get_db().write_dbtip(new_tip);
}

// Members are destroyed in reverse declaration order: invoke_txes_ goes
// first, so each leftover InvokeTX releases its BlockTX reference (possibly
// garbage collecting an abandoned block through remove_shutdown_block_tx(),
// whose state is declared before invoke_txes_ and is therefore still alive),
// and only then are the remaining BlockTXs destroyed.
RPCContext::~RPCContext() = default;

} // namespace hogd
