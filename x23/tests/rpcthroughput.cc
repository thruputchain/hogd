
#include <atomic>
#include <netinet/in.h>
#include <unistd.h>

#include <utility>
#include <x23/refcnt.h>
#include <x23/rpc.h>
#include <x23/sockio.h>

#include "tests/xdrtest.hh"

using namespace x23;

std::string progname;
unsigned opt_threads = (nproc() + 1) / 2;
unsigned opt_connections;
unsigned opt_async_rpcs_per_thread = 10;
unsigned opt_seconds = 1;

#if NDEBUG
#define make_rpc_stream rpc_stream
#else // !NDEBUG
#define make_rpc_stream rpc_stream_logged
#endif // !NDEBUG

struct Listener : DestroyInhibit {
  CoroPoll &cp_;
  UniqueFd lfd_;
  sockaddr_storage addr_{};
  std::atomic_int count_{};

  Listener(CoroPoll &cp, int port = 0) : cp_(cp), lfd_(tcplisten(port))
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
  Detached accept_loop(this RefPtr<Listener> self SLOC_PARM);
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
Listener::accept_loop(this RefPtr<Listener> self SLOC_ARG)
{
  sockaddr_storage ss;
  socklen_t socklen;
  while (auto s = co_await self->cp_.accept(self->lfd_, &ss, &socklen)) {
    std::println("accepted fd {} TCP port {} from {}", *s,
                 get_port(self->addr_), HostPort(ss));
    make_rpc_stream(self->cp_, *s).server(std::make_unique<Server>(self));
  }
  std::println("server stopping");
}

std::atomic<std::uint16_t> server_port{0};

void
do_server(int port)
{
  ThreadPool pool(opt_threads);
  CoroPoll cp(pool);
  Listener listener(cp, port);
  server_port = get_port(listener.addr_);
  if (port == 0)
    std::println("port {}", server_port.load());
  server_port.notify_all();

  pause();
  exit(0);
}

void
do_client(int port)
{
  std::atomic_int count{0};

  ThreadPool pool(opt_threads);
  CoroPoll cp(pool);
  auto server_addr = *HostPort{"localhost", uformat("{}", port)}.to_addr(true);

  auto make_client = [&](unsigned ncoros) -> Detached {
    auto conn = co_await cp.tcpconnect(&server_addr);
    if (!conn) {
      std::println(stderr, "tcpconnect: {}", conn.error());
      co_return;
    }
    std::println("connected fd {} TCP port {} to {}", *conn,
                 get_port(local_addr(*conn)), HostPort(server_addr));
    auto c = make_rpc_stream(cp, *conn).template client<blank_rpc_v1>();
    for (unsigned i = 0; i < ncoros; ++i)
      [](auto c, std::atomic_int &count) -> Detached {
        try {
          for (;;) {
            co_await c.null();
            ++count;
          }
        } catch (const XdrException &e) {
        }
        co_return;
      }(c, count);
  };

  for (int i = opt_connections; i > 0; --i)
    make_client(opt_async_rpcs_per_thread);

  sleep(opt_seconds);

  std::println("{} RPCs", count.load());
}

[[noreturn]]
void
usage(int err)
{
  std::println(err ? stderr : stdout,
               "usage: {} [-C|-S] [-a #coros/thread] [-t #threads]\n"
               "       {:{}} [-c #connections] [-s seconds] [port]",
               progname, "", progname.size());
  exit(err);
}

static void
parse_int_opt(std::integral auto &value, const char *s = optarg) noexcept
{
  auto [ptr, ec] = std::from_chars(s, s + strlen(s), value);
  if (ec != std::errc()) {
    std::println(stderr, "{}: cannot parse integer {}", progname, s);
    exit(2);
  }
}

int
main(int argc, char **argv)
{
  progname =
      std::filesystem::path(argc > 0 ? argv[0] : __FILE__).filename().string();

  char mode{};
  std::uint16_t port = 0;

  int opt;
  while ((opt = getopt(argc, argv, "CSt:c:a:s:")) != -1)
    switch (opt) {
    case 'C':
    case 'S':
      mode = opt;
      break;
    case 'h':
      usage(0);
      break;
    case 't':
      parse_int_opt(opt_threads);
      break;
    case 'c':
      parse_int_opt(opt_connections);
      break;
    case 'a':
      parse_int_opt(opt_async_rpcs_per_thread);
      break;
    case 's':
      parse_int_opt(opt_seconds);
      break;
    default:
      usage(2);
    }

  if (optind < argc)
    parse_int_opt(port, argv[optind++]);
  if (optind != argc) {
    std::println(stderr, "{}: too many arguments", progname);
    usage(2);
  }

  if (!opt_connections)
    opt_connections = opt_threads;

  setlinebuf(stdout);
#if X23_CORO_TRACE
  if (getenv("COROTRACE"))
    corotrace.enable_ = 2;
#endif // !X23_CORO_TRACE
  if (getenv("RPCTRACE"))
    RpcLogger::default_logfd = 1;

  if (mode == 'C')
    do_client(port);
  else if (mode == 'S')
    do_server(port);
  else if (!mode) {
    std::thread{do_server, 0}.detach();
    while (!server_port.load())
      server_port.wait(0);
    do_client(server_port.load());
  }
  else
    usage(2);

  return 0;
}
