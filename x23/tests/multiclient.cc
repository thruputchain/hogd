#include <gtest/gtest.h>

#include "x23/rpc.h"
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
    auto [port, fd] = tcp_rpc_server(cp, 0, [&impl](auto c) {
        c->server(&impl, blank_rpc_v1{});
    });

    auto client = RpcMulticlient<RpcLogger>::make(cp, *HostPort{"localhost", port}.to_addr(true), *llc_map);

    auto result = client->template client<blank_rpc_v1>().echo(Data{1, 2, 3})();
    auto expect = Data{1, 2, 3};
    EXPECT_TRUE(result == expect);
}

}

