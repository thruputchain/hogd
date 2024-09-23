#include <print>

#include <sys/socket.h>

#include "tests/xdrtest.hh"
#include "x23/rpc.h"

using namespace std::string_literals;
//using namespace x23;

x23::ThreadPool threadpool(2);
x23::CoroPoll cp(threadpool);

struct Context {};
Context context;

using x23::Async;

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
};
static_assert(is_myvers<Impl *, Async, Context>);

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

  co_return;
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
      myvers::client c = cs.client<myvers>();
      client(c)();
      return 0;
    }
    else if (argv[1] == "-s"s) {
      [argv]->Async<void> {
	auto lfd = x23::tcplisten(std::stoi(argv[2]));
	while (auto c = co_await cp.accept(lfd)) {
	  rpc_stream_logged(cp, *c).server<myvers>(&impl, context);
	}
      }()();
    }
    else {
      std::println(stderr, "usage: {} [{{-c | -s}} port]", argv[0]);
      return 2;
    }
  }

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
    x23::Errno{}.raise("socketpair");

  rpc_stream_logged(cp, fds[1]).server<myvers>(&impl, context);

  auto cs = rpc_stream_logged(cp, fds[0]);
  myvers::client c = cs.client<myvers>();
  client(c)();
}
