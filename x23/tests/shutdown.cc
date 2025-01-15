#include <gtest/gtest.h>

#include <string>

#include "x23/event.h"
#include "x23/threadpool.h"
#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/fileio.h"
#include "x23/rpc.h"

namespace x23 {

namespace {

constexpr static std::string kPort = "10234";

struct Connection {
	x23::ThreadPool pool_;
	x23::CoroPoll cp_;

	Connection(int nthreads)
		: pool_(nthreads)
		, cp_(pool_) {}


	void start() {

	  auto lfd = x23::tcplisten(atoi(kPort.c_str()));
	  if (!lfd) {
	    perror("listen");
	    exit(EXIT_FAILURE);
	  }

	  pool_.spawn([this, lfd{std::move(lfd)}] -> x23::Detached {
	    while (auto c = co_await cp_.accept(lfd)) {
	      x23::rpc_stream_logged(cp_, *c).server(this);
	    }
	  });
	}

	using rpc_service = blank_rpc_v1;
	x23::Async<void> null() {
		co_return;
	}
};

static_assert(is_blank_rpc_v1<Connection *, x23::Async>);

}

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

  // This forces the threadpool to wait until a connection is formed (i.e. after the background
  // work begins running) before attempting to shutdown the coropoll.
  auto client = x23::rpc_stream_logged(cp, *fd).template client<blank_rpc_v1>();
  client.null()();
}

}