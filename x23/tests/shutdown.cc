#include <gtest/gtest.h>

#include <string>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/fileio.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace x23 {

namespace {

constexpr static std::string kPort = "10234";

struct Connection {
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;

  Connection(int nthreads) : pool_(nthreads), cp_(pool_) {}

  void start()
  {
    auto lfd = x23::tcplisten(atoi(kPort.c_str()));
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    [](x23::CoroPoll &cp, x23::ThreadPool &pool, Connection *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await pool.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, pool_, this, std::move(lfd));
  }

  using rpc_service = blank_rpc_v1;
  x23::Async<void> null() { co_return; }
};

static_assert(is_blank_rpc_v1<Connection *, x23::Async>);
} // namespace

TEST(ShutdownTest, AfterRPC)
{
  Connection c(1);

  c.start();

  x23::ThreadPool pool(1);
  x23::CoroPoll cp(pool);

  auto fd = cp.tcpconnect("localhost", kPort)();
  if (!fd) {
    exit(-1);
  }

  // This forces the threadpool to wait until a connection is formed (i.e. after
  // the background work begins running) before attempting to shutdown the
  // coropoll.
  auto client = x23::rpc_stream_logged(cp, *fd).template client<blank_rpc_v1>();
  client.null()();
}
} // namespace x23
