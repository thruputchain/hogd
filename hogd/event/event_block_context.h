#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <vector>

#include "event.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "indexer.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

using indexer_event_client_t =
    IndexerEvent1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

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
    // Ensure the work_directory_ exists before creating the events file.
    auto dir = Directory::open(work_directory_);
    if (!dir) {
      panic("failed to open work directory: {}", dir.error());
    }
    auto filename = temp_events_filename(work_directory_, block_id_);
    auto fd = open(filename.c_str(), O_CREAT | O_WRONLY, 0666);
    if (fd == -1) {
      panic("failed to open events file: {}", strerror(errno));
    }
    events_file_ = x23::UniqueFd(fd);
  }

  ~EventBlockContext();

  x23::Async<void> add_event(TxEventLog event);
  SHA256Hash get_root_hash();
  x23::Async<void> finish_file_write();
  x23::Async<void> finish_file(SHA256Hash const &hash);

  x23::Async<void> await_abandon();

  x23::Async<void>
  send_events_to_indexers(BlockHeight block_height,
                          std::vector<indexer_event_client_t> &indexers)
  {
    TxEventLogBatch full_event_log;
    {
      std::unique_lock ul = event_log_.lock_threads();
      event_log_.for_each(ul, [&full_event_log](
                                  TxEventLogBatch &per_thread_event_log) {
        full_event_log.append(
          std::make_move_iterator(per_thread_event_log.begin()),
          std::make_move_iterator(per_thread_event_log.end())
        );
        per_thread_event_log.clear();
      });
    }

    std::vector<x23::Async<void>> handles;
    handles.reserve(indexers.size());

    for (indexer_event_client_t &indexer : indexers) {
      handles.push_back(
          indexer.indexer_event_process_events(block_height, full_event_log));
    }
    for (x23::Async<void> &handle : handles) {
      co_await handle;
    }
  }

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

  x23::Async<void> await_pending_writes_done();

  std::unique_ptr<MerkleTriePrefix> trie_;
  std::string work_directory_;
  BlockID block_id_;

  PerThread<TxEventLogBatch> event_log_;

  struct EventContext {
    std::vector<uint8_t> buf;
    uint32_t event_count;
  };

  PerThread<EventContext> buffers_;

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

  x23::UniqueFd events_file_;
};

} // namespace hogd
