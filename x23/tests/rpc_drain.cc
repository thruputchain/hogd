// A server that shuts itself down in response to an RPC has to send
// that RPC's reply before it exits, or the client sees EOF instead of
// the reply.  See issue #597.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

using namespace x23;
using namespace std::chrono_literals;

namespace {

// What hogd's CI control server does in its shutdown RPC: tell the main
// thread to tear everything down, then take a while getting back with
// the reply.
struct ShutdownFlag {
  std::atomic<bool> stopped_{false};

  void request()
  {
    stopped_ = true;
    stopped_.notify_all();
    // The real handler kills subprocesses and joins threads before it
    // returns, so the teardown reliably gets there first.
    std::this_thread::sleep_for(50ms);
  }
  void wait() { stopped_.wait(false); }
};

// Replies without suspending, so the reply goes out from the reader
// coroutine itself.
struct SyncService {
  using rpc_service = blank_rpc_v1;

  ShutdownFlag stop_;

  void null() { stop_.request(); }
  static Data echo(Data arg) { return arg; }
};

// Suspends first, so the reply goes out from a coroutine of its own,
// after the reader has moved on.
struct AsyncService {
  using rpc_service = blank_rpc_v1;

  ThreadPool &pool_;
  ShutdownFlag stop_;

  AsyncService(ThreadPool &pool) : pool_(pool) {}

  Async<void> null()
  {
    co_await pool_.bg();
    stop_.request();
  }
  static Async<Data> echo(Data arg) { co_return arg; }
};

// Runs the client half in its own thread, leaving the test thread free
// to play the part of the server's main().
class Client {
public:
  Client(std::uint16_t port) : thread_([this, port] { run(port); }) {}

  bool got_reply() const { return got_reply_; }
  const std::string &error() const { return error_; }

private:
  std::atomic<bool> got_reply_{false};
  std::string error_;
  std::jthread thread_;

  void run(std::uint16_t port)
  {
    ThreadPool pool(1);
    CoroPoll cp(pool);
    auto fd = cp.tcpconnect("localhost", std::to_string(port))();
    if (!fd) {
      error_ = fd.error().what();
      return;
    }
    auto client = rpc_stream(cp, *fd)->client(blank_rpc_v1{});
    try {
      client.null()();
      got_reply_ = true;
    } catch (const std::exception &e) {
      error_ = e.what();
    }
  }
};

} // namespace

TEST(RpcDrainTest, ShutdownWaitsForInFlightReply)
{
  // conns must outlive the CoroPoll: connections leave the set as they
  // fail, and destroying the CoroPoll is what makes them fail.
  RpcConnSet conns;
  auto pool = std::make_unique<ThreadPool>(2);
  auto cp = std::make_unique<CoroPoll>(*pool);
  SyncService impl;
  auto [port, lfd] = tcp_rpc_server(*cp, 0, [&conns, &impl](auto c) {
    conns.insert(*c);
    c->server(&impl);
  });

  Client client(port);
  impl.stop_.wait();

  // The server's shutdown path.
  cp->poison(lfd);
  conns.drain();
  cp.reset();
  pool.reset();

  EXPECT_TRUE(client.got_reply()) << client.error();
}

TEST(RpcDrainTest, ShutdownWaitsForSuspendedReply)
{
  RpcConnSet conns;
  auto pool = std::make_unique<ThreadPool>(2);
  auto cp = std::make_unique<CoroPoll>(*pool);
  AsyncService impl(*pool);
  auto [port, lfd] = tcp_rpc_server(*cp, 0, [&conns, &impl](auto c) {
    conns.insert(*c);
    c->server(&impl);
  });

  Client client(port);
  impl.stop_.wait();

  cp->poison(lfd);
  conns.drain();
  cp.reset();
  pool.reset();

  EXPECT_TRUE(client.got_reply()) << client.error();
}
