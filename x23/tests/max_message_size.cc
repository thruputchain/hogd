#include <gtest/gtest.h>

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/threadpool.h"

namespace x23 {

struct SyncServer {
  using rpc_service = blank_rpc_v1;
  void null() {}
  Data echo(Data arg) { return arg; }
};
static_assert(is_blank_rpc_v1<SyncServer *, std::type_identity_t>);

struct AsyncServer {
  using rpc_service = blank_rpc_v1;
  static Async<void> null() { co_return; }
  static Async<Data> echo(Data arg) { co_return arg; }
};
static_assert(is_blank_rpc_v1<AsyncServer *, Async>);

TEST(MaxMessageSize, SrpcClientRejectsOversizedMessage)
{
  SyncServer server;

  UniqueFd listener = stcplisten(0);
  ASSERT_TRUE(listener);
  uint16_t port = get_port(local_addr(listener));
  UniqueFd client_fd = tcpconnect("127.0.0.1", port);
  ASSERT_TRUE(client_fd);
  UniqueFd server_fd(::accept(listener.get(), nullptr, nullptr));
  ASSERT_TRUE(server_fd);

  srpc_stream(server_fd.release())->server(&server);

  RefPtr<SRpcStream<>> client_stream{*new SRpcStream<>(std::move(client_fd))};
  client_stream->max_message_size_ = 100;
  client_stream->go();
  auto client = client_stream->client(blank_rpc_v1{});

  Data big(200, 0xaa);
  EXPECT_THROW(client.echo(big), XdrBadInput);

  Data small{1, 2, 3};
  EXPECT_EQ(client.echo(small), small);
}

TEST(MaxMessageSize, AsyncRpcClientRejectsOversizedMessage)
{
  AsyncServer server;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  rpc_stream(cp, fds[1])->server(&server);

  auto cc = rpc_stream(cp, fds[0]);
  cc->max_message_size = 100;
  auto client = cc->client<blank_rpc_v1>();

  Data big(200, 0xaa);
  EXPECT_THROW(client.echo(big)(), XdrBadInput);

  Data small{1, 2, 3};
  EXPECT_EQ(client.echo(small)(), small);
}

// Server whose echo handler suspends on an externally-resumable
// awaitable, so we can control exactly when the handler resumes.
struct SuspendedEchoServer {
  using rpc_service = blank_rpc_v1;

  SuspendedEchoServer() : resume_(response_) {}

  Async<void> null() { co_return; }

  Async<Data> echo(Data /*arg*/)
  {
    invoked_.store(true, std::memory_order_release);
    invoked_.notify_all();
    co_return co_await response_;
  }

  void wait_until_invoked()
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!invoked_.load(std::memory_order_acquire)) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      invoked_.wait(false);
    }
  }

  void resume(Data response) { resume_(std::move(response)); }

  std::atomic_bool invoked_{false};
  Awaitable<Data> response_;
  CoroCB<Data> resume_;
};
static_assert(is_blank_rpc_v1<SuspendedEchoServer *, Async>);

// Regression test for issue #3116: a suspended async server handler must
// not observe its RpcDispatcher freed if the stream fails while the
// handler is suspended.  Previously RpcStreamImpl::fail() cleared
// srvmap_, destroying the dispatcher out from under the detached handler
// continuation and causing a heap-use-after-free on resume (caught by
// ASan).  With the fix, the dispatcher outlives the handler and resuming
// it is safe.  This is a plain functional test (not a death test): under
// ASan the unfixed code aborts here; the fixed code runs to completion.
TEST(RpcDispatcherLifetime, SuspendedHandlerSurvivesStreamFail)
{
  SuspendedEchoServer server;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  auto server_stream = rpc_stream_logged(cp, fds[1], -1);
  server_stream->server(&server);
  server_stream->go();

  auto client_stream = rpc_stream_logged(cp, fds[0], -1);
  auto client = client_stream->client<blank_rpc_v1>();
  // Eagerly starts, sending the call and suspending on the reply.
  auto pending_call = client.echo(Data{0x41, 0x42, 0x43});

  // Wait until the server handler is running and suspended on response_.
  server.wait_until_invoked();

  // Fail the stream while the handler is suspended mid-dispatch.
  server_stream->fail();

  // Resume the handler.  It runs its reply/log path through the
  // dispatcher (inline on this thread), which must still be alive.  On
  // the unfixed code this is where the heap-use-after-free happens.
  server.resume(Data{0x44, 0x45, 0x46});

  // Let any pool-thread tail of the continuation settle.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Drain the still-suspended client call before teardown: the server's
  // reply never reached us (its fd was poisoned), so the call is parked
  // in the client stream's xidmap_.  Failing the client stream resumes
  // it with an empty buffer, completing it with an error we discard.
  // This keeps the coroutine frame alive until it is unregistered, so
  // destroying it at teardown cannot dangle.
  client_stream->fail();
  try {
    pending_call();
  } catch (const std::exception &) {
    // Expected: the stream failed, so the call cannot complete normally.
  }

  // Reaching here without an ASan abort or crash is the regression check.
  SUCCEED();
}

} // namespace x23
