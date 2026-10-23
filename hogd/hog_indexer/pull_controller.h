#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "hogd/hog_indexer/shared.h"

namespace hogd {

class IndexDB;
class RecentTxFailureCache;
struct IndexerOutboxOrigin;

struct IndexerPullControllerOptions {
  std::chrono::milliseconds retry_delay = std::chrono::milliseconds(100);
  // Pause after a caught-up reply before asking again. The event server
  // answers the first caught-up pull at a progress immediately and parks a
  // repeat of it until the next publication, so the re-ask is what arms the
  // park and there is nothing to wait for first. Nonzero only matters against
  // a server that answers every caught-up pull eagerly.
  std::chrono::milliseconds caught_up_poll_delay = std::chrono::milliseconds(0);
};

// Serves IndexerEvent1 on one connection to the event server, collecting the
// chunks of the block that connection's chunked pull is answering. Pulls on a
// connection are sequential and the event server replies only after every
// chunk is acknowledged, so it holds one block's chunks at a time.
class IndexerChunkSink {
public:
  using rpc_service = IndexerEvent1;

  void indexer_event_block_chunk(EventIndexerBlockChunk chunk);

  // The batches of the block a chunked pull answered with [header], once
  // all of its chunks are in; nullopt if any is missing. Clears the sink.
  std::optional<IndexerCommittedBlockPayload>
  take_block(const EventIndexerChunkedBlock &header);

private:
  std::mutex mutex_;
  std::vector<std::optional<EventIndexerBlockChunk>> chunks_;
  // The block the chunks belong to, set by the first.
  BlockHeight height_ = -1;
  IndexerCommittedBlockIdentity identity_{};
};

// Owns the standalone indexer's event connection and durable replay loop.
class IndexerPullController {
public:
  IndexerPullController(std::string event_server_addr, IndexDB &db,
                        RecentTxFailureCache &failure_cache,
                        IndexerPullControllerOptions options = {});
  ~IndexerPullController();

  IndexerPullController(const IndexerPullController &) = delete;
  IndexerPullController &operator=(const IndexerPullController &) = delete;

  void wait_until_caught_up();
  bool wait_until_caught_up_for(std::chrono::milliseconds timeout);

private:
  void run(const std::stop_token &stop);
  bool connect_once();
  bool pull_tx_failures(event_indexer_client_t &client);
  // The block a chunked pull answered, assembled from the chunks [sink]
  // collected before the reply.
  static EventIndexerPulledBlock
  assemble_block(IndexerChunkSink &sink, EventIndexerChunkedBlock header);
  bool initialize_origin_if_needed(const IndexerOutboxOrigin &origin);
  bool wait(std::stop_token stop, std::chrono::milliseconds delay);
  void fail_event_client();
  void mark_caught_up();
  void mark_not_ready();
  // True once this connection has been answered caught up and nothing has
  // since marked it not ready. Such a pull sends its cursor in the CaughtUp
  // state so the server holds it until the next publication.
  bool caught_up_now();
  void reset_event_client();

  std::string event_server_addr_;
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
  IndexerPullControllerOptions options_;
  std::mutex event_client_mutex_;
  std::optional<event_indexer_client_t> event_client_;
  // Serves the chunks of event_client_'s pulls; replaced with it.
  std::shared_ptr<IndexerChunkSink> chunk_sink_;
  std::mutex wait_mutex_;
  std::condition_variable_any wake_;
  bool caught_up_ = false;
  bool ever_caught_up_ = false;
  std::atomic<bool> stopping_ = false;
  std::jthread thread_;
};

} // namespace hogd
