#include <gtest/gtest.h>

#include <cstdint>

#include "x23/async.h"
#include "x23/config.h"
#include "x23/cpumap.h"
#include "x23/event.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"
#include "x23/vec.h"
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

TEST(RpcMulticlient, ConstructorWithExplicitIpv4Bind)
{
  TestServer impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);
  DestroyInhibit di;

  auto bind_addr = HostPort{"127.0.0.1", 0}.to_addr();
  ASSERT_TRUE(bind_addr.has_value());

  auto [port, fd] =
      tcp_rpc_server(cp, *bind_addr, [&impl, _d = RefPtr(di)](auto c) {
        c->server(&impl, blank_rpc_v1{});
      });

  auto client_addr = HostPort{"127.0.0.1", port}.to_addr();
  ASSERT_TRUE(client_addr.has_value());

  auto client = RpcMulticlient<RpcLogger>::make(cp, *client_addr, *llc_map);
  auto result = client->template client<blank_rpc_v1>().echo(Data{4, 5, 6})();
  EXPECT_EQ(result, (Data{4, 5, 6}));

  cp.poison(fd);
  di.destroy_wait();
}

TEST(RpcMulticlient, ConstructorWithExplicitIpv6Bind)
{
  auto bind_addr = HostPort{"::1", 0}.to_addr();
  if (!bind_addr.has_value())
    GTEST_SKIP() << "IPv6 loopback is unavailable";

  TestServer impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);
  DestroyInhibit di;

  auto [port, fd] =
      tcp_rpc_server(cp, *bind_addr, [&impl, _d = RefPtr(di)](auto c) {
        c->server(&impl, blank_rpc_v1{});
      });

  auto client_addr = HostPort{"::1", port}.to_addr();
  ASSERT_TRUE(client_addr.has_value());

  auto client = RpcMulticlient<RpcLogger>::make(cp, *client_addr, *llc_map);
  auto result = client->template client<blank_rpc_v1>().echo(Data{7, 8, 9})();
  EXPECT_EQ(result, (Data{7, 8, 9}));

  cp.poison(fd);
  di.destroy_wait();
}

// Building a map per CPU needs the affinity API.
#if HAVE_SCHED_SETAFFINITY
TEST(RpcMulticlient, PlacementPicksEachConnectionsCoroPollAndReaderCpu)
{
  TestServer impl;
  ThreadPool pool(4);
  CoroPoll server_cp(pool);
  CoroPoll cp0(pool);
  CoroPoll cp1(pool);
  DestroyInhibit di;

  auto [port, fd] =
      tcp_rpc_server(server_cp, 0, [&impl, _d = RefPtr(di)](auto c) {
        c->server(&impl, blank_rpc_v1{});
      });
  auto addr = HostPort{"localhost", port}.to_addr(true);
  ASSERT_TRUE(addr.has_value());

  // One bucket per CPU we may run on, so every bucket is valid.
  Vec<int> buckets(CpuSet::num_cpus(), 0);
  int nbuckets = 0;
  for (unsigned cpu = 0; cpu < CpuSet::num_cpus() && nbuckets < 4; ++cpu)
    if (allowed_cpus->isset(cpu))
      buckets[cpu] = nbuckets++;
  if (nbuckets < 2)
    GTEST_SKIP() << "needs two allowed CPUs";
  CpuMap map{buckets};

  using Placement = RpcMulticlient<RpcLogger>::Placement;
  int calls = 0;
  auto place = [&](const CpuSet &cpus) {
    unsigned first = 0;
    while (!cpus.isset(first))
      ++first;
    return Placement{(calls++ % 2) ? cp1 : cp0, first};
  };
  auto client = RpcMulticlient<RpcLogger>::make(place, *addr, map);
  EXPECT_EQ(calls, map.nbuckets());

  for (int i = 0; i < map.nbuckets(); ++i) {
    auto &stream = client->lookup(i);
    EXPECT_EQ(stream->cp_.get(), (i % 2) ? &cp1 : &cp0) << "bucket " << i;
    auto result = stream->client(blank_rpc_v1{})
                      .echo(Data{static_cast<std::uint8_t>(i)})();
    EXPECT_EQ(result, (Data{static_cast<std::uint8_t>(i)})) << "bucket " << i;
  }

  server_cp.poison(fd);
  di.destroy_wait();
}
#endif // HAVE_SCHED_SETAFFINITY

} // namespace x23
