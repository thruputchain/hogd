
#include <atomic>
#include <charconv>
#include <chrono>
#include <poll.h>
#include <utility>

#include <netinet/in.h>
#include <unistd.h>

#include <x23/refcnt.h>
#include <x23/rpc.h>
#include <x23/sockio.h>

#include "tests/xdrtest.hh"

using namespace x23;

std::string progname;
std::shared_ptr<ThreadPool> pool;
bool opt_csv;
unsigned opt_connections;
unsigned opt_async_rpcs_per_thread = 10;
unsigned opt_seconds = 1;
std::optional<std::uint32_t> opt_data;

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

  Listener(CoroPoll &cp, int port = 0)
    : cp_(cp), lfd_(tcplisten(port, opt_connections > 5 ? opt_connections : 5))
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

  Async<Data> echo(Data arg) { co_return arg; }
};
static_assert(is_blank_rpc_v1<Server *, Async>);

Detached
Listener::accept_loop(this RefPtr<Listener> self SLOC_ARG)
{
  sockaddr_storage ss;
  socklen_t socklen;
  while (auto s = co_await self->cp_.accept(self->lfd_, &ss, &socklen)) {
    //    std::println("accepted fd {} TCP port {} from {}", *s,
    //                 get_port(self->addr_), HostPort(ss));
    make_rpc_stream(self->cp_, *s).server(std::make_unique<Server>(self));
  }
  std::println("server stopping");
}

std::atomic<std::uint16_t> server_port{0};

void
do_server(int port, bool quiet)
{
  auto retain_pool(pool);
  CoroPoll cp(*retain_pool);
  Listener listener(cp, port);
  server_port = get_port(listener.addr_);
  if (port == 0 && !quiet)
    std::println("port {}", server_port.load());
  server_port.notify_all();

  pause();
  exit(0);
}

void
do_client(int port, const char *server)
{
  std::atomic_int count{0};

  CoroPoll cp(*pool);
  auto server_addr =
      *HostPort{server ? server : "localhost", uformat("{}", port)}.to_addr(
          true);

  auto make_client = [&](unsigned ncoros) -> Detached {
    auto conn = co_await cp.tcpconnect(&server_addr);
    if (!conn) {
      std::println(stderr, "tcpconnect: {}", conn.error());
      co_return;
    }
    //    std::println("connected fd {} TCP port {} to {}", *conn,
    //                 get_port(local_addr(*conn)), HostPort(server_addr));
    auto c = make_rpc_stream(cp, *conn).template client<blank_rpc_v1>();

    if (opt_data) {
      for (unsigned i = 0; i < ncoros; ++i)
        [](auto c, std::atomic_int &count) -> Detached {
          Data bytes(size_t(*opt_data), std::uint8_t(0xc5));
          try {
            for (;;) {
              co_await c.echo(bytes);
              ++count;
            }
          } catch (const XdrException &e) {
          }
          co_return;
        }(c, count);
    }
    else
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

  // Warm up for 100 msec
  poll(nullptr, 0, 100);

  using namespace std::chrono;
  auto start = high_resolution_clock::now();
  count = 0;
  sleep(opt_seconds);
  auto final_count = count.load();
  auto elapsed = high_resolution_clock::now() - start;

  double rate = double(final_count) / duration<double>(elapsed).count();

  if (opt_csv)
    std::println("{},{},{},{},{},{:.0f}", server ? server : "INTERNAL",
                 opt_data ? std::string{} : std::to_string(*opt_data),
                 pool->nthreads_, opt_connections, opt_async_rpcs_per_thread,
                 rate);
  else
    std::println("{}: {}, {} threads, {} connections, {} coros/connection\n"
                 "    {} RPCs / {} = {:.0f} RPCs/sec",
                 server ? server : "INTERNAL",
                 opt_data ? std::format("{} bytes", *opt_data)
                          : std::string("no data"),
                 pool->nthreads_, opt_connections, opt_async_rpcs_per_thread,
                 final_count, elapsed, rate);
}

[[noreturn]]
void
usage(int err)
{
  std::println(err ? stderr : stdout,
               "usage: {} [-C|-S] [-q] [-a #coros/thread] [-t #threads]\n"
               "       {:{}} [-c #connections] [-s seconds] [-d bytes] [port]",
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

  char opt_mode{};
  std::uint16_t port{};
  unsigned opt_threads{};
  const char *server{};

  int opt;
  while ((opt = getopt(argc, argv, "CSqt:c:a:s:d:")) != -1)
    switch (opt) {
    case 'C':
    case 'S':
      opt_mode = opt;
      break;
    case 'h':
      usage(0);
      break;
    case 'q':
      opt_csv = true;
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
    case 'd':
      parse_int_opt(opt_data.emplace());
      break;
    case 's':
      parse_int_opt(opt_seconds);
      break;
    default:
      usage(2);
    }

  if (optind < argc)
    parse_int_opt(port, argv[optind++]);
  if (optind < argc && opt_mode == 'C')
    server = argv[optind++];
  if (optind != argc) {
    std::println(stderr, "{}: too many arguments", progname);
    usage(2);
  }

  if (!opt_threads)
    opt_threads = opt_mode ? nproc() : (nproc() + 1) / 2;
  pool = std::make_shared<ThreadPool>(opt_threads);
  if (!opt_connections)
    opt_connections = opt_threads;

  setlinebuf(stdout);
#if X23_CORO_TRACE
  if (getenv("COROTRACE"))
    corotrace.enable_ = 2;
#endif // !X23_CORO_TRACE
  if (getenv("RPCTRACE"))
    RpcLogger::default_logfd = 1;

  if (opt_mode == 'C') {
    if (!port) {
      std::println(stderr, "Must specify port number with -C");
      exit(2);
    }
    do_client(port, server ? server : "localhost");
  }
  else if (opt_mode == 'S')
    do_server(port, false);
  else if (!opt_mode) {
    std::thread{do_server, 0, true}.detach();
    while (!server_port.load())
      server_port.wait(0);
    do_client(server_port.load(), nullptr);
  }
  else
    usage(2);

  return 0;
}
