#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "hogd/xdr/protocol_token.h"
#include "indexer.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/rpc_common.h"
#include "x23/threadpool.h"

// Coverage for the kRpcNoCatch form of RpcServer (hogd/base/rpc_server.h).
//
// A plain RpcServer registers its impl with RpcStream::server(), which turns
// an exception out of a handler into a SYSTEM_ERR reply: the caller's one
// call fails and the node keeps running.  RpcServer(kRpcNoCatch, ...)
// registers with nocatch_server() instead, so the exception escapes and
// terminates the process at the fault, with a core dump, rather than
// papering over it.
//
// The x23 side of this is covered in x23/tests/rpc_nocatch.cc.  What is
// specific to hogd, and what is checked here, is the plumbing: the
// deduction guides have to route kRpcNoCatch to NoCatch = true, and
// start_accepting() has to pick nocatch_server() for every accepted
// connection.  Both are silent if they break -- a server would simply go on
// catching -- so they are pinned by a compile-time check on the deduced type
// and a runtime check that a handler throw really is fatal.

namespace hogd {
namespace {

// Also used verbatim as the death-test regex, so keep it free of characters
// a POSIX extended regex would read as operators.
constexpr const char kBoom[] = "rpc server nocatch test boom";

class ThrowingPullRpc {
public:
  using rpc_service = EventIndexer1;

  static EventIndexerPullResult
  event_indexer_pull_next(const EventIndexerPullRequest & /*request*/)
  {
    throw std::runtime_error(kBoom);
  }

  static IndexerTxFailureNoticeBatch event_indexer_pull_tx_failures()
  {
    return {};
  }
};
static_assert(is_EventIndexer1<ThrowingPullRpc *, x23::MaybeAsync>);

class NotReadyPullRpc {
public:
  using rpc_service = EventIndexer1;

  static x23::Async<EventIndexerPullResult>
  event_indexer_pull_next(EventIndexerPullRequest /*request*/)
  {
    co_return EventIndexerPullResult(
        EventIndexerPullOutcome::EventIndexerPullNotReady);
  }

  static IndexerTxFailureNoticeBatch event_indexer_pull_tx_failures()
  {
    return {};
  }
};
static_assert(is_EventIndexer1<NotReadyPullRpc *, x23::MaybeAsync>);

// The kRpcNoCatch tag has to reach the class's third template argument, and
// only the deduction guides carry it there.  Deducing the wrong thing costs
// nothing at the call site -- you still get a working server -- so assert on
// the deduced types directly.
static_assert(std::is_same_v<decltype(RpcServer(NotReadyPullRpc{})),
                             RpcServer<NotReadyPullRpc, EventIndexer1, false>>);
static_assert(std::is_same_v<decltype(RpcServer(std::declval<x23::CoroPoll &>(),
                                                NotReadyPullRpc{})),
                             RpcServer<NotReadyPullRpc, EventIndexer1, false>>);
static_assert(
    std::is_same_v<decltype(RpcServer(kRpcNoCatch, NotReadyPullRpc{})),
                   RpcServer<NotReadyPullRpc, EventIndexer1, true>>);
static_assert(std::is_same_v<decltype(RpcServer(kRpcNoCatch,
                                                std::declval<x23::CoroPoll &>(),
                                                NotReadyPullRpc{})),
                             RpcServer<NotReadyPullRpc, EventIndexer1, true>>);

EventIndexerPullRequest
test_request()
{
  EventIndexerPullRequest request;
  request.protocol = protocol_from_text("test");
  return request;
}

// Connects to a server on the loopback port and returns the stream, whose
// lifetime the caller owns: the client has to be torn down before the
// RpcServer, which drains its connections on the way out.
auto
connect_client(x23::CoroPoll &cp, std::uint16_t port)
{
  auto fd = cp.tcpconnect("127.0.0.1", std::to_string(port))();
  if (!fd.has_value())
    // The host/port overload reports failure as a std::system_error.
    throw std::system_error(fd.error());
  return x23::rpc_stream_logged(cp, *fd, -1);
}

TEST(RpcServerNoCatch, CatchingServerReportsHandlerThrow)
{
  x23::CoroPoll &cp = default_coropoll();
  RpcServer<ThrowingPullRpc> server(cp);
  const std::uint16_t port = server.start_server(0);

  auto client_stream = connect_client(cp, port);
  auto client = client_stream->client<EventIndexer1>();

  try {
    (void)client.event_indexer_pull_next(test_request())();
    ADD_FAILURE() << "the pull returned instead of failing";
  } catch (const x23::XdrCallFailed &e) {
    EXPECT_EQ(x23::msg_show_stat(e.m_), "SYSTEM_ERR");
  }

  // The handler failed, not the node: the connection is still good.
  EXPECT_TRUE(client.event_indexer_pull_tx_failures()().empty());
}

// kRpcNoCatch changes only what a throwing handler does.  This is the
// check that start_accepting() still builds a working server when it takes
// the nocatch_server() branch.
TEST(RpcServerNoCatch, NoCatchServerServesSuccessfulCalls)
{
  x23::CoroPoll &cp = default_coropoll();
  RpcServer server(kRpcNoCatch, cp, NotReadyPullRpc{});
  const std::uint16_t port = server.start_server(0);

  auto client_stream = connect_client(cp, port);
  auto client = client_stream->client<EventIndexer1>();

  EventIndexerPullResult result =
      client.event_indexer_pull_next(test_request())();
  EXPECT_EQ(result.outcome(),
            EventIndexerPullOutcome::EventIndexerPullNotReady);
  EXPECT_TRUE(client.event_indexer_pull_tx_failures()().empty());
}

// Everything the death test touches is built inside the child: a fork would
// not carry the coroutine thread pool's threads across.  It is a function
// rather than an inline block so the EXPECT_DEATH macro is not handed a
// statement full of commas.
void
run_handler_throw()
{
  x23::ThreadPool pool(2);
  x23::CoroPoll cp(pool);
  RpcServer server(kRpcNoCatch, cp, ThrowingPullRpc{});
  const std::uint16_t port = server.start_server(0);

  auto client_stream = connect_client(cp, port);
  auto client = client_stream->client<EventIndexer1>();

  try {
    (void)client.event_indexer_pull_next(test_request())();
  } catch (const std::exception &) {
    // Reached only if the throw still became a SYSTEM_ERR reply.  Returning
    // from here is how gtest gets to report that nothing died; letting the
    // exception escape would terminate the child and pass the death test for
    // the wrong reason.
    std::fputs("the handler exception came back as an rpc reply\n", stderr);
  }
}

TEST(RpcServerNoCatchDeathTest, HandlerThrowTerminatesProcess)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(run_handler_throw(), kBoom);
}

} // namespace
} // namespace hogd
