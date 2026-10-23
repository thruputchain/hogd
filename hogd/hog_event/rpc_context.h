#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_conflict.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/thread_checked_mutex.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/event_block_context.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/hog_event/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"

namespace hogd {

class EventRPCContext {
public:
  explicit EventRPCContext(std::string_view work_directory);

  // Wakes every indexer pull parked on the next publication so it answers
  // now. ~EventIndexerServer calls this before RpcServer drains: destruction
  // waits for in-flight handlers but cannot cancel a suspended one, and a
  // parked pull is exactly that.
  void release_indexer_pulls();

  // Acquires a reference to the PerThreadARC<EventBlockContext> for the given
  // block id.
  // Returns nullptr if the block is not open.
  x23::Async<PerThreadARCRef<EventBlockContext>> get_context(BlockID block_id);

  // Opens a new block.
  // Returns an error if the block already exists.
  //
  // The block's temp events file is created outside lock_, between two short
  // critical sections: the first reserves the id, the second inserts the
  // context. Nothing under lock_ touches the disk on this path.
  x23::Async<EventStatus<void>> open_block(BlockID block_id);

  // Closes a block.
  // Returns the root hash of the block if successful.
  // Returns an error if the block is not open.
  x23::Async<EventStatus<SHA256Hash>> close_block(BlockID block_id,
                                                  BlockHeight block_height);

  // Durably publishes a closed block and GCs the block context.
  // Returns the block height on success.
  x23::Async<EventStatus<BlockHeight>>
  commit_block(BlockID block_id, ConsensusValue consensus_value);

  x23::Async<EventStatus<void>>
  reconcile_outbox_tip(BlockID block_id, ConsensusValue consensus_value);

  // Abandons a block (active or already closed but not yet committed).
  // Returns an error if the block_id is not found.
  x23::Async<EventStatus<void>> abandon_block(BlockID block_id);

  // This is normally called by commit_block() or abandon_block().
  // However, if the block is abandoned, it might be called by some caller
  // that called get_context(), if the block is abandoned while an outstanding
  // rpc from the executor is in flight.
  x23::Async<void> gc_context(BlockID block_id);

  // [draining] belongs to the RpcServer serving the request. Its destructor
  // sets it and then calls release_indexer_pulls(); from then on a pull that
  // would park is answered instead, so one arriving after the release cannot
  // park on a count nothing will move past and pin the server's teardown.
  x23::Async<EventIndexerPullResult>
  pull_indexer_next(const EventIndexerPullRequest &request,
                    const std::atomic<bool> &draining);
  // For callers with no server to drain.
  x23::Async<EventIndexerPullResult>
  pull_indexer_next(const EventIndexerPullRequest &request);

  void enqueue_tx_failure(TransactionHash tx_hash,
                          TxFailureDiagnostic diagnostic);
  IndexerTxFailureNoticeBatch pull_tx_failures();

  const IndexerOutboxStore &indexer_outbox_store() const
  {
    return indexer_outbox_store_;
  }

  void set_close_reserved_test_hook(std::function<void(BlockID)> hook)
  {
    close_reserved_test_hook_ = std::move(hook);
  }
  static constexpr size_t kMaxTxFailureNotices = 256;

private:
  enum class BlockOperation { kOpening, kClosing, kCommitting, kAbandoning };

  x23::Async<PerThreadARC<EventBlockContext> *>
  get_context_slow(BlockID block_id);
  // Tears down a settled block's context.  Caller holds lock_.
  //
  // Returns the block's events file descriptor, if it still has one, for the
  // caller to close after releasing lock_ (see close_events_file_async).  The
  // name may just have been unlinked by release_events_file(); with the
  // descriptor open that only dropped the directory entry, and the kernel
  // frees the file's blocks at this close.  For a large losing block that is
  // milliseconds of filesystem work, which must not happen under the lock
  // every open_block() waits on.
  [[nodiscard]] x23::UniqueFd gc_context_internal(
      std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator
          it);
  // Closes [events_file] on a thread outside the pool, since the close can
  // block for milliseconds; see ThreadPool::async.
  static x23::Detached close_events_file_async(x23::UniqueFd events_file);

  // Records that a block published its events file under [hash].  Caller holds
  // lock_.
  void retain_events_file(SHA256Hash const &hash);
  // Drops one block's claim on [hash], unlinking the file when the last one
  // goes away.  Caller holds lock_.
  void release_events_file(SHA256Hash const &hash);

  // Bumps indexer_publications_ and schedules its waiters. The tail path
  // calls it under outbox_commit_lock_ so a handler's sample and commit's bump
  // are ordered; the notice path needs no lock, since a parked handler checks
  // the mailbox after sampling.
  void publish_indexer_wake();

  x23::Async<bool> reserve_block_operation(BlockID block_id,
                                           BlockOperation operation);
  x23::Async<void>
  finish_block_operation(BlockID block_id,
                         PerThreadARC<EventBlockContext> *context,
                         bool shutdown);

  struct ContextCache {
    ContextCache() = default;
    ContextCache(ContextCache &&cache) noexcept
      : contexts_(std::move(cache.contexts_))
    {}

#ifdef NDEBUG
    using mutex_type = std::mutex;
#else
    using mutex_type = ThreadCheckedMutex<std::mutex>;
#endif

    mutex_type mtx_;
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext> *> contexts_;
  };

  x23::CoroLock lock_;
  // Serializes origin selection, committed publication, and pullable-tail
  // snapshots. Pull payload I/O is always performed after releasing it.
  x23::CoroLock outbox_commit_lock_;
  std::atomic<bool> outbox_reconciled_ = false;
  std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>
      event_block_contexts_;
  std::unordered_map<BlockID, BlockOperation> block_operations_;
  // Live claims on each published <event root>.events file, guarded by lock_.
  //
  // The name is content-addressed, so it is shared by every block with the
  // same event root -- all empty blocks, at every height, and any two
  // identical nominations.  A block therefore cannot reclaim "its" file on its
  // own: abandoning an empty block at one height would delete the file a
  // closed-but-uncommitted empty block at another height still has to read at
  // commit.  A claim is taken when a block publishes the file and dropped when
  // its context is torn down, whether that is via commit or via abandonment.
  //
  // In-memory only.  Files that survive a crash have no claims, and reclaiming
  // those is startup reconciliation's job.
  std::unordered_map<SHA256Hash, size_t, SipHash24Hasher> events_file_refs_;
  // Guarded by outbox_commit_lock_. Every record through this finalized
  // height is pullable, including a crash-window record one ahead of HogDB.
  std::optional<BlockHeight> finalized_indexer_tail_;
  std::optional<IndexerOutboxOrigin> current_indexer_origin_;
  std::vector<IndexerOutboxEpochOriginInfo> indexer_origins_;

  // Publication counter that caught-up indexer pulls park on. Bumped and
  // broadcast under outbox_commit_lock_ whenever the finalized tail moves, and
  // once more on release. A parked handler re-reads the tail on wake: a moved
  // tail is served, an unmoved one is answered rather than re-parked.
  x23::CoroFutex<std::uint64_t> indexer_publications_{0};

  std::mutex tx_failure_mutex_;
  std::vector<IndexerTxFailureNotice> tx_failure_mailbox_;

  PerThread<ContextCache> cache_;
  // In any normal workflow we almost certainly are not seeing more than a
  // couple open blocks at a time.
  constexpr static size_t kMaxCachedBlocks = 5;
  std::vector<ReusableEventBlockContextData> cached_blocks_;
  std::string work_directory_;
  IndexerOutboxStore indexer_outbox_store_;
  std::function<void(BlockID)> close_reserved_test_hook_;
};

} // namespace hogd
