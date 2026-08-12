#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "indexer.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {
namespace {

struct SuspendedIndexerPullState {
  SuspendedIndexerPullState() : resume(response) {}

  std::binary_semaphore invoked{0};
  x23::Awaitable<EventIndexerPullResult> response;
  x23::CoroCB<EventIndexerPullResult> resume;
};

class SuspendedIndexerPullRpc {
public:
  using rpc_service = EventIndexer1;

  explicit SuspendedIndexerPullRpc(
      std::shared_ptr<SuspendedIndexerPullState> state)
    : state_(std::move(state))
  {}

  x23::Async<EventIndexerPullResult>
  event_indexer_pull_next(EventIndexerPullRequest /*request*/)
  {
    state_->invoked.release();
    co_return co_await state_->response;
  }

  static IndexerTxFailureNoticeBatch event_indexer_pull_tx_failures()
  {
    return {};
  }

private:
  std::shared_ptr<SuspendedIndexerPullState> state_;
};
static_assert(is_EventIndexer1<SuspendedIndexerPullRpc *, x23::MaybeAsync>);

// Characterizes the RpcServer lifetime behavior after #3307.
//
// A suspended server handler keeps the RpcStream alive. The stream owns its
// RpcDispatcher, and RpcServer's dispatcher wrapper owns an ImplPtr reference
// back to the RpcServer. During RpcServer destruction, poisoning the accepted
// fd stops the stream reader but does not cancel the suspended handler, so the
// ImplPtr remains live and destroy_wait() cannot return until application code
// independently resumes the handler.
//
// This is not necessarily a bug if RpcServer destruction intentionally drains
// all outstanding RPC handlers. It is useful coverage for the lifetime
// contract introduced by #3307 and a building block for the stronger case
// where the requesting peer has disconnected but unrelated async work still
// prevents the handler from completing.
//
// Use EventIndexer1::event_indexer_pull_next because the production
// implementation is itself an async handler. Suspend on an explicit Awaitable
// here so the test is deterministic and does not depend on I/O timing.
TEST(RpcServerShutdown, SuspendedIndexerPullPinsServerDestruction)
{
  auto state = std::make_shared<SuspendedIndexerPullState>();
  x23::CoroPoll &cp = default_coropoll();
  auto server = std::make_unique<RpcServer<SuspendedIndexerPullRpc>>(cp, state);
  const std::uint16_t port = server->start_server(0);

  auto fd = cp.tcpconnect("127.0.0.1", std::to_string(port))();
  ASSERT_TRUE(fd.has_value());
  auto client_stream = x23::rpc_stream_logged(cp, *fd, -1);
  auto client = client_stream->client<EventIndexer1>();

  EventIndexerPullRequest request;
  request.protocol = "test";
  auto pending_call = client.event_indexer_pull_next(request);

  ASSERT_TRUE(state->invoked.try_acquire_for(std::chrono::seconds(5)));

  std::binary_semaphore shutdown_done{0};
  std::thread shutdown_thread([&] {
    server.reset();
    shutdown_done.release();
  });

  // Record the graceful-drain semantics: destruction remains blocked while the
  // handler is live, then completes once application-level work finishes.
  const bool completed = shutdown_done.try_acquire_for(std::chrono::seconds(1));
  EXPECT_FALSE(completed)
      << "RpcServer destruction stopped waiting for the suspended handler";

  if (!completed) {
    EventIndexerPullResult result(
        EventIndexerPullOutcome::EventIndexerPullNotReady);
    result.diagnostic() = "test complete";
    state->resume(std::move(result));
    shutdown_done.acquire();
  }
  shutdown_thread.join();

  // The server-side stream is gone, so fail the client explicitly to drain the
  // outstanding call before its coroutine frame is destroyed at test teardown.
  client_stream->fail();
  EXPECT_THROW((void)pending_call(), std::exception);
}

} // namespace
} // namespace hogd
