#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <utility>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"
#include "x23/vec.h"
#include "x23/x23.h"

using namespace std::string_literals;
// using namespace x23;

x23::ThreadPool threadpool(2);
x23::CoroPoll cp(threadpool);

struct Context {};
Context context;

using x23::Async;
using x23::MaybeAsync;

struct Impl {
  using rpc_service = myvers;
  Async<void> null(Context &c)
  {
    std::println("serving null");
    co_return;
  }
  Async<int> takes_int(int i, Context &c)
  {
    std::println("serving takes_int");
    co_return i + 1;
  }
  Async<int> takes_two_ints(int a, int b, Context &c)
  {
    std::println("serving takes_two_ints");
    co_return a + b;
  }

  Async<coloru> takes_recursive(test_recursive, fix_12 b, Context &c)
  {
    std::println("serving takes_recursive");
    coloru ret;
    ret.c(color::RED);
    ret.r() = "This is the string in the result";
    co_return ret;
  }

  Async<x23::span<std::int32_t>> takes_vec(intvec v, Context &c)
  {
    static intvec ret = std::move(v);
    co_return ret;
  }
};
static_assert(is_myvers<Impl *, Async, Context>);

static_assert(
    x23::satisfies_rpc_return<Async<x23::span<int32_t>>, intvec, MaybeAsync>);
static_assert(is_myvers<Impl *, MaybeAsync, Context>);

Impl impl;

Async<void>
client(auto &c)
{
  int i;
  co_await c.null();
  std::println("null returned");
  i = co_await c.takes_int(5);
  std::println("takes_int(5) = {}", i);
  i = co_await c.takes_two_ints(7, 8);
  std::println("takes_two_ints(7,8) = {}", i);

  uptr up(true);
  up.val().emplace(3);
  // std::println("{}", show(up));

  test_recursive tr{.elem = "outer element"};
  tr.nextvec.push_back({.elem = "vec element"});
  coloru cu = co_await c.takes_recursive(tr, fix_12{.i = 99, .d = 95.3});

  intvec iv{1, 2, 3};
  co_await c.takes_vec(iv);
  x23::span<const int32_t> is(iv);
  co_await c.takes_vec(is);

  co_return;
}

void
make_server(x23::RpcStream<x23::RpcLogger>::Configurator c)
{
  c->server(&impl, myvers{}, context);
}

int
main(int argc, char **argv)
{
  setlinebuf(stdout);
  x23::RpcLogger::default_logfd = 1;

  if (argc >= 3) {
    if (argv[1] == "-c"s) {
      auto conres = cp.tcpconnect("localhost", argv[2])();
      if (!conres) {
        std::println(stderr, "{}", conres.error().what());
        exit(1);
      }
      auto cs = rpc_stream_logged(cp, *conres);
      myvers::client c = cs->client<myvers>();
      client(c)();
      return 0;
    }
    else if (argv[1] == "-s"s)
      tcp_rpc_server_wait(cp, std::stoi(argv[2]),
                          [](auto c) { c->server(&impl, myvers{}, context); });
    else {
      std::println(stderr, "usage: {} [{{-c | -s}} port]", argv[0]);
      return 2;
    }
  }

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
    x23::Errno{}.raise("socketpair");

  rpc_stream_logged(cp, fds[1])->server<myvers>(&impl, context);

  auto cs = rpc_stream_logged(cp, fds[0]);
  myvers::client c = cs->client<myvers>();
  client(c)();
}
