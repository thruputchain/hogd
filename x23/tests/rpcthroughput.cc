
#include <atomic>
#include <charconv>
#include <chrono>
#include <utility>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <x23/refcnt.h>
#include <x23/rpc.h>
#include <x23/sockio.h>

#include "tests/xdrtest.hh"

using namespace x23;

std::string progname;
bool opt_csv;
unsigned opt_connections;
unsigned opt_async_rpcs_per_thread = 10;
unsigned opt_seconds = 1;
unsigned opt_threads;
unsigned opt_pools = 1;
std::optional<std::uint32_t> opt_data;

#if NDEBUG
#define make_rpc_stream rpc_stream
#else // !NDEBUG
#define make_rpc_stream rpc_stream_logged
#endif // !NDEBUG

// CoroPoll and ThreadPool
struct CpTp {
  ThreadPool pool_;
  CoroPoll cp_;
  CpTp(size_t nthreads) : pool_(nthreads), cp_(pool_) {}

  struct Vec : x23::Vec<std::unique_ptr<CpTp>> {
    using super = x23::Vec<std::unique_ptr<CpTp>>;
    using super::super;
    CoroPoll &get(BorrowedFd fd) { return (*this)[fd.get() % size()]->cp_; }
  };

  static Vec make_vec(size_t nthreads, size_t ncptp);
};

struct Listener : DestroyInhibit {
  CpTp::Vec &cps_;
  UniqueFd lfd_;
  sockaddr_storage addr_{};
  std::atomic_int count_{};

  Listener(CpTp::Vec &cps, int port = 0)
    : cps_(cps), lfd_(tcplisten(port, opt_connections + 5))
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
    cps_.get(0).reset(lfd_.get());
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

CpTp::Vec
CpTp::make_vec(size_t nthreads, size_t ncptp)
{
  assert(ncptp > 0 && nthreads >= ncptp);

  Vec ret;
  ret.reserve(ncptp);
  size_t mintpp = nthreads / ncptp;
  size_t extratpp = nthreads % ncptp;
  for (size_t i = 0; i < ncptp; ++i)
    ret.emplace_back(new CpTp{mintpp + size_t(i < extratpp)});
  return ret;
}

Detached
Listener::accept_loop(this RefPtr<Listener> self SLOC_ARG)
{
  sockaddr_storage ss;
  socklen_t socklen;
  while (auto s =
             co_await self->cps_.get(0).accept(self->lfd_, &ss, &socklen)) {
    //    std::println("accepted fd {} TCP port {} from {}", *s,
    //                 get_port(self->addr_), HostPort(ss));
    make_rpc_stream(self->cps_.get(*s), *s)
        .server(std::make_unique<Server>(self));
  }
  std::println("server stopping");
}

std::atomic<std::uint16_t> server_port{0};

void
do_server(int port, bool quiet)
{
  auto cps = CpTp::make_vec(opt_threads, opt_pools);
  Listener listener(cps, port);
  server_port = get_port(listener.addr_);
  if (port == 0 && !quiet)
    std::println("port {}", server_port.load());
  server_port.notify_all();

  pause();
  exit(0);
}

// Format an int with commas, because {:L} doesn't seem to work yet.
std::string
formatL(uint64_t v)
{
  std::string ret, in = std::format("{}", v);
  for (size_t i = 0, e = in.size(); i != e; ++i) {
    if ((e - i) % 3 == 0 && i != 0)
      ret += ',';
    ret += in[i];
  }
  return ret;
}

void
do_client(int port, const char *server)
{
  auto &cps = *new CpTp::Vec{CpTp::make_vec(opt_threads, opt_pools)};
  std::atomic_int count{0};

  auto server_addr =
      *HostPort{server ? server : "localhost", uformat("{}", port)}.to_addr(
          true);

  using DiRef = RefPtr<DestroyInhibit>;

  auto make_client = [&](DiRef dir, unsigned ncoros) -> Detached {
    UniqueFd fd{socket(server_addr.ss_family, SOCK_STREAM, 0)};
    if (!fd) {
      std::println(stderr, "socket: {}", Errno{});
      co_return;
    }
    if (auto res = co_await cps.get(fd).connect(fd, &server_addr); !res) {
      std::println(stderr, "connect(fd {}): {}", fd.get(), res.error());
      co_return;
    }

    // std::println("connected fd {} TCP port {} to {}", fd.get(),
    // 		 get_port(local_addr(fd)), HostPort(server_addr));
    auto &cp = cps.get(fd);
    auto c = make_rpc_stream(cp, fd.release()).template client<blank_rpc_v1>();

    if (opt_data) {
      for (unsigned i = 0; i < ncoros; ++i)
        [](DiRef dir, auto c, std::atomic_int &count) -> Detached {
          Data bytes(size_t(*opt_data), std::uint8_t(0xc5));
          try {
            for (;;) {
              co_await c.echo(bytes);
              ++count;
            }
          } catch (const XdrException &e) {
            // std::println("Excpetion: {}", e.what());
          }
          co_return;
        }(dir, c, count);
    }
    else
      for (unsigned i = 0; i < ncoros; ++i)
        [](DiRef dir, auto c, std::atomic_int &count) -> Detached {
          try {
            for (;;) {
              co_await c.null();
              ++count;
            }
          } catch (const XdrException &e) {
            // std::println("Excpetion: {}", e.what());
          }
          co_return;
        }(dir, c, count);
  };

  DestroyInhibit inh;
  for (int i = opt_connections; i > 0; --i)
    make_client(inh, opt_async_rpcs_per_thread);

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
                 opt_threads, opt_connections, opt_async_rpcs_per_thread, rate);
  else
    std::println(
        "{}: {}, {} pool{}, {} threads, {} connections, {} coros/connection\n"
        "    {} RPCs / {} = {} RPCs/sec",
        server ? server : "INTERNAL",
        opt_data ? std::format("{} bytes", *opt_data) : std::string("no data"),
        opt_pools, opt_pools == 1 ? "" : "s", opt_threads, opt_connections,
        opt_async_rpcs_per_thread, final_count, elapsed, formatL(rate));
  delete &cps;
  inh.destroy_wait();
}

[[noreturn]]
void
usage(int err)
{
  std::println(err ? stderr : stdout,
               "usage: {} [-C|-S] [-q] [-a #coros/thread] [-t #threads]\n"
               "       {:{}} [-c #connections] [-s seconds] [-d bytes]\n"
               "       [port [server]]",
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
  const char *server{};

  int opt;
  while ((opt = getopt(argc, argv, "CSqt:c:a:s:d:p:")) != -1)
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
    case 'p':
      parse_int_opt(opt_pools);
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
  if (opt_threads < opt_pools)
    opt_threads = opt_pools;

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
