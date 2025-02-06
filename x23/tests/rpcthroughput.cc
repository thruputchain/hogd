
#include <netinet/in.h>

#include <x23/refcnt.h>
#include <x23/rpc.h>
#include <x23/sockio.h>

#include "tests/xdrtest.hh"

using namespace x23;

ThreadPool pool;
CoroPoll cp(pool);

struct Listener : DestroyInhibit {
  CoroPoll &cp_;
  UniqueFd lfd_;
  sockaddr_storage addr_{};
  std::atomic_int count_{};

  Listener(CoroPoll &cp) : cp_(cp), lfd_(tcplisten(0))
  {
    if (!lfd_)
      throw Errno{};
    sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(get_port(local_addr(lfd_)));
    memcpy(&addr_, &sin, sizeof(sin));
    accept_loop();
  }

  ~Listener()
  {
    std::println("destroying server");
    cp_.reset(lfd_.get());
    lfd_.reset();
    std::println("destroy_wait");
    destroy_wait();
    std::println("server destroyed");
  }

private:
  Detached accept_loop(this RefPtr<Listener> self);
};

struct Server {
  using rpc_service = blank_rpc_v1;

  RefPtr<Listener> l_;
  Server(RefPtr<Listener> l) : l_(std::move(l)) {}

  Async<void> null()
  {
    l_->count_++;
    co_return;
  }
};
static_assert(is_blank_rpc_v1<Server *, Async>);

Detached
Listener::accept_loop(this RefPtr<Listener> self)
{
  sockaddr_storage ss;
  socklen_t socklen;
  while (auto s = co_await self->cp_.accept(self->lfd_, &ss, &socklen)) {
    std::println("accepted fd {} TCP port {} from {}", *s,
		 get_port(self->addr_), HostPort(ss));
    rpc_stream_logged(self->cp_, *s).server(std::make_unique<Server>(self));
  }
  std::println("server stopping");
}

int
main()
{
  // RpcLogger::default_logfd = 1;
  Listener listener(cp);
  std::stop_source ss;
  for (int i = 0; i < 10; ++i)
    [](RefPtr<Listener> l, std::stop_token st) -> Detached {
      auto conn = co_await l->cp_.tcpconnect(&l->addr_);
      if (!conn) {
        std::println(stderr, "tcpconnect: {}", conn.error());
        co_return;
      }
      std::println("connected fd {} TCP port {} to {}", *conn,
                   get_port(local_addr(*conn)), HostPort(l->addr_));
      auto c = rpc_stream_logged(l->cp_, *conn).template client<blank_rpc_v1>();
      while (!st.stop_requested())
        co_await c.null();
      std::println("client stopping");
    }(listener, ss.get_token());
  sleep(1);
  auto count = listener.count_.load();
  ss.request_stop();
  std::println("count: {}", count);
}
