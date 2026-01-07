#include <gtest/gtest.h>

#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/event.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"
#include "xdrtest.hh"

namespace x23 {

struct TestServer {
  using rpc_service = blank_rpc_v1;

  static Async<void> null() { co_return; }

  static Async<Data> echo(Data arg) { co_return arg; }
};
static_assert(is_blank_rpc_v1<TestServer *, Async>);

TEST(RpcMulticlient, Constructor)
{
  TestServer impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);
  DestroyInhibit di;

  auto [port, fd] = tcp_rpc_server(cp, 0, [&impl, _d = RefPtr(di)](auto c) {
    c->server(&impl, blank_rpc_v1{});
  });

  HostPort hp{"localhost", port};
  auto sockaddr = hp.to_addr(true);
  ASSERT_TRUE(sockaddr.has_value());
  auto sockaddr_storage = sockaddr.value();

  auto client = RpcMulticlient<RpcLogger>::make(cp, sockaddr_storage, *llc_map);

  auto result = client->template client<blank_rpc_v1>().echo(Data{1, 2, 3})();
  auto expect = Data{1, 2, 3};
  EXPECT_TRUE(result == expect);
  cp.poison(fd);
  di.destroy_wait();
}

} // namespace x23
