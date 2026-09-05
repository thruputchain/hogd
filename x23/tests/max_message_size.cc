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

// Reproduces issue #3116 (currently FAILING; no fix applied here).
//
// A suspended async server handler holds, via the copied ProcHandler in
// its detached continuation, a bare reference to its RpcDispatcher.  If
// the stream fails while the handler is suspended, RpcStreamImpl::fail()
// runs srvmap_.clear() and destroys that dispatcher; when the handler
// resumes it dereferences the freed dispatcher (RpcLogger::log_server
// reads the freed logfd_), a remote-triggerable heap-use-after-free.
//
// This is a plain functional test (not a death test): built with
// -fsanitize=address it aborts with a heap-use-after-free at
// server.resume() below on the current (unfixed) code, and should run to
// completion once the lifetime bug is fixed.
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

// A single ONC RPC record marker leaves 31 bits for the payload length, so a
// message-size cap above kMaxRecordPayloadSize cannot be framed in one
// fragment.  Creating a client with such a cap must fail loudly at setup
// rather than let make_record() later truncate the marker and desync the
// stream (issue #3275).
TEST(MaxMessageSize, ClientCreationRejectsCapAboveRecordLimit)
{
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  UniqueFd server_fd{fds[1]};

  auto cc = rpc_stream(cp, fds[0]);
  cc->max_message_size = kMaxRecordPayloadSize + 1;
  EXPECT_THROW(cc->client<blank_rpc_v1>(), XdrBadInput);
}

} // namespace x23
