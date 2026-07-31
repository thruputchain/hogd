#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <utility>
#include <vector>

#include "event.hh"
#include "event_outbox_internal.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/perthread.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "indexer.hh"
#include "proposal.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

using indexer_event_client_t =
    IndexerEvent1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

namespace detail {
struct EventContext {
  std::vector<uint8_t> buf;
  uint32_t event_count = 0;
};

// An event log paired with the transaction body that produced it.  The two are
// stored together in a single PerThread container (rather than in two separate
// PerThread<T> containers) so they are always flattened in one shared traversal
// order at commit.  Keeping them in independent PerThread containers allowed
// the per-index body/event association to cross, because PerThread<T> registers
// threads in a type-local order that can differ between T's (see issue #2826).
struct EventWithBody {
  TxEventLog event;
  ProposedTransaction body;
};

} // namespace detail

using EventWithBodyBatch = std::vector<detail::EventWithBody>;

struct ReusableEventBlockContextData {
  ReusableEventBlockContextData(
      std::unique_ptr<MerkleTriePrefix> trie,
      PerThread<EventWithBodyBatch> events_with_bodies,
      PerThread<detail::EventContext> buffers)
    : trie(std::move(trie)), events_with_bodies(std::move(events_with_bodies)),
      buffers(std::move(buffers))
  {}

  std::unique_ptr<MerkleTriePrefix> trie;
  PerThread<EventWithBodyBatch> events_with_bodies;
  PerThread<detail::EventContext> buffers;

  ReusableEventBlockContextData(
      ReusableEventBlockContextData &&other) noexcept = default;
  ReusableEventBlockContextData &
  operator=(ReusableEventBlockContextData &&other) noexcept = default;

  ReusableEventBlockContextData(ReusableEventBlockContextData const &) = delete;
  ReusableEventBlockContextData &
  operator=(ReusableEventBlockContextData const &) = delete;
};

class EventBlockContext {
public:
  static std::string temp_events_filename(std::string_view work_directory,
                                          BlockID block_id)
  {
    return std::string(work_directory) + "/" + std::to_string(block_id) +
           ".events_temp";
  }
  static std::string final_events_filename(std::string_view work_directory,
                                           SHA256Hash hash)
  {
    return std::string(work_directory) + "/" +
           encode_hex(std::as_bytes(std::span(hash))) + ".events";
  }

  EventBlockContext(std::string_view work_directory, BlockID block_id)
    : trie_(std::make_unique<MerkleTriePrefix>(
          /*alocator_type=*/Allocator<Node>::Type::kBump)),
      work_directory_(work_directory), block_id_(block_id)
  {
    open_events_file();
  }

  EventBlockContext(std::string_view work_directory, BlockID block_id,
                    ReusableEventBlockContextData data);

  ReusableEventBlockContextData extract_data();

  ~EventBlockContext();

  // Returns false if no executor handle is open (i.e. executor_open_block()
  // was not called). Events must only be emitted while an executor handle is
  // held.
  //
  // Keep [body] paired with its event log for the durable indexer outbox. It is
  // not hashed into the consensus event trie.
  x23::Async<bool> add_event(TxEventLog event, ProposedTransaction body);
  SHA256Hash get_root_hash();
  x23::Async<void> finish_file_write();
  x23::Async<void> finish_file(SHA256Hash const &hash);

  IndexerOutboxCandidatePayload
  snapshot_indexer_outbox_candidate(BlockHeight block_height,
                                    SHA256Hash event_root,
                                    ProposalTxs proposal_txs);

  x23::Async<void> wait_file_done();
  void delete_events_file();

  void set_block_height(BlockHeight h)
  {
    CHECK_EQ(block_height_, -1);
    block_height_ = h;
  }
  BlockHeight block_height() const
  {
    CHECK_NE(block_height_, -1);
    return block_height_;
  }

  x23::Async<void> await_abandon();
  x23::Async<bool> is_abandoned()
  {
    x23::CoroGuard g;
    co_await g(lock_);
    co_return state_ == BlockState::kAbandoned;
  }

  // Executor handle tracking - count of executors with open handles to this
  // block
  void executor_open_block();
  // Fail-stops (CHECK) on a close with no open handle: block ids are never
  // reused, so an unmatched close is an executor bug, and letting the count
  // underflow would wedge await_all_executors_closed() forever.
  void executor_close_block();
  // Waits for all executors to close.
  // Returns true if all executors closed normally, false if block was abandoned
  x23::Async<bool> await_all_executors_closed();

  void set_closing() { closing_.store(true, std::memory_order_release); }
  bool closing() { return closing_.load(std::memory_order_acquire); }

private:
  x23::Async<void> write_event(std::span<const uint8_t> data);

  // Flush the current thread's buffer to the file.
  // WILL PANIC if the block has begun shutting down
  // (i.e. if finish_file_write() has been called).
  // That would indicate a race between the block finishing
  // and a call to add_event().  We must synchronize those calls,
  // so that we do not nondeterministically exclude events from the block.
  x23::Detached flush_buffer_async(std::vector<uint8_t> buf);
  void sync_file(x23::CoroGuard const &g);
  // The rename is slow if there's already a file of the same hash.
  // (the time to delete the previous file).
  // In actual practice, it should be impossible for this to occur,
  // as the output files contain transaction hashes (which are uniquified)
  // unless multiple replicas make the exact same nomination.  We could deal
  // with this edge case if this becomes a problem, but I suspect this will not
  // matter significantly.
  void rename_file(x23::CoroGuard const & /*unused*/, SHA256Hash const &hash);

  static x23::ThreadPool &get_threadpool()
  {
    auto *threadpool = x23::ThreadPool::get_thread_pool();
    if (!threadpool) {
      threadpool = &default_threadpool();
    }
    return *threadpool;
  }

  std::atomic<bool> closing_ = false;

  x23::Async<void> await_pending_writes_done();
  void open_events_file();

  std::unique_ptr<MerkleTriePrefix> trie_;
  std::string work_directory_;
  BlockID block_id_;

  // Keep event logs and transaction bodies paired while per-thread batches are
  // flattened into the durable candidate snapshot (#2826).
  PerThread<EventWithBodyBatch> events_with_bodies_;
  PerThread<detail::EventContext> buffers_;

  // State transitions:
  // kActive -> kFinishing -> kFinished
  // kActive -> kAbandoned
  //
  // Finishing or Abandoned signals the add_event method
  // to stop dispatching async event writes.
  enum class BlockState : int32_t {
    kActive,
    kAbandoned,
    kFinishing,
    kFinished,
  };

  x23::CoroLock lock_;
  BlockState state_ = BlockState::kActive;
  uint32_t pending_write_count_ = 0;

  x23::CoroFutex<bool> all_writes_done_ = false;

  // Executor handle tracking - count of executors with open handles
  std::atomic<uint32_t> open_executor_handles_ = 0;
  x23::CoroFutex<bool> all_executors_closed_ = false;

  x23::UniqueFd events_file_;
  BlockHeight block_height_ = -1;
  SHA256Hash root_hash_{};
  bool has_root_hash_ = false;
  x23::CoroFutex<bool> file_done_ = false;
};

} // namespace hogd
