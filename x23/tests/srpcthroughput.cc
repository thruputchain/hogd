
#include <atomic>
#include <charconv>
#include <chrono>

#include <sys/poll.h>

#include <x23/cleanup.h>
#include <x23/fastclock.h>
#include <x23/mvar.h>
#include <x23/sockio.h>
#include <x23/srpc.h>
#include <x23/threadpool.h>

#include "tests/latency.h"
#include "tests/xdrtest.hh"

#if NDEBUG
#define make_rpc_stream srpc_stream
#else // !NDEBUG
#define make_rpc_stream srpc_stream_logged
#endif // !NDEBUG

using namespace x23;

std::string progname;
bool opt_csv;
unsigned opt_connections;
bool opt_latency{};
unsigned opt_seconds = 1;
unsigned opt_threads_per_connection = 1;
bool opt_multiclient = false;
std::optional<std::uint32_t> opt_data;

std::atomic_uint16_t server_port{0};
std::atomic_bool start_counting{false};
std::atomic_uint64_t rpc_count{0};

MVar<LatencyStats<>> latency;

struct Server {
  using rpc_service = blank_rpc_v1;

  std::atomic_int nserved_{0};

  void null() { nserved_.fetch_add(1, std::memory_order_relaxed); }
  Data echo(Data arg)
  {
    nserved_.fetch_add(1, std::memory_order_relaxed);
    return arg;
  }
};
static_assert(is_blank_rpc_v1<Server *, std::type_identity_t>);

Server server;

void
do_server(std::uint16_t port, bool quiet)
{
  UniqueFd lfd = stcplisten(port, opt_connections + 5);
  server_port = get_port(local_addr(lfd));
  server_port.notify_all();
  if (port == 0 && !quiet)
    std::println("port {}", server_port.load());

  for (;;) {
    sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    UniqueFd fd{accept(lfd.get(), reinterpret_cast<sockaddr *>(&ss), &sslen)};
    if (!fd)
      std::println(stderr, "accept: {}", Errno{});
    else {
      // std::println("accepted fd {} TCP port {} from {}", fd.get(),
      //             server_port.load(), HostPort(ss));
      srpc_stream_logged(fd.release())->server(&server);
    }
  }
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

template<bool DATA, bool LATENCY>
inline void
do_call_loop(std::stop_token st, auto client)
{
  auto bytes = make_if<DATA, Data>(opt_data.value_or(0), 0xc7);
  auto ls = make_if<LATENCY, LatencyStats<>>();
  auto start = make_if<LATENCY, fastclock::time_point>();
  uint64_t ctr = 0;
  try {
    while (!st.stop_requested() && !start_counting) {
      if constexpr (DATA)
        client.echo(bytes);
      else
        client.null();
    }
    while (!st.stop_requested()) {
      if constexpr (LATENCY)
        start = fastclock::now();
      if constexpr (DATA)
        client.echo(bytes);
      else
        client.null();
      if constexpr (LATENCY)
        ls.sample(fastclock::now() - start);
      ++ctr;
    }
  } catch (std::exception &e) {
    std::println("{}", e.what());
  }
  rpc_count += ctr;
  if constexpr (LATENCY)
    latency->operator+=(ls);
}

void
call_loop(std::stop_token st, auto client)
{
  if (opt_data.has_value()) {
    if (opt_latency)
      do_call_loop<true, true>(std::move(st), std::move(client));
    else
      do_call_loop<true, false>(std::move(st), std::move(client));
  }
  else {
    if (opt_latency)
      do_call_loop<false, true>(std::move(st), std::move(client));
    else
      do_call_loop<false, false>(std::move(st), std::move(client));
  }
}

template<bool MULTI = false>
void
do_client(int port, const char *server)
{
  auto server_addr =
      *HostPort{server ? server : "localhost", uint16_t(port)}.to_addr(true);
  std::stop_source ss;

  std::vector<std::jthread> threads;

  if constexpr (!MULTI) {
    for (unsigned i = 0; i < opt_connections; ++i) {
      UniqueFd fd;
      try {
        fd = tcpconnect(server_addr);
      } catch (std::exception &e) {
        std::println("{}", e.what());
        continue;
      }
      auto client = make_rpc_stream(fd.release())->client(blank_rpc_v1{});
      for (unsigned j = 0; j < opt_threads_per_connection; ++j)
        threads.emplace_back(call_loop<decltype(client)>, ss.get_token(),
                             client);
    }
  }
  else {
    auto client =
        SRpcMulticlient<RpcLogger>::make(server_addr)->client(blank_rpc_v1{});
    for (int n = opt_connections * opt_threads_per_connection; n > 0; --n)
      threads.emplace_back(call_loop<decltype(client)>, ss.get_token(), client);
  }

  // Warm up for 100 msec
  poll(nullptr, 0, 100);
  start_counting = true;

  using namespace std::chrono;
  auto start = high_resolution_clock::now();
  sleep(opt_seconds);
  ss.request_stop();
  auto elapsed = high_resolution_clock::now() - start;
  threads.clear();

  double rate = rpc_count / duration<double>(elapsed).count();

  if (opt_csv)
    std::print("{},{},{},{},{:.0f}", server ? server : "INTERNAL",
               opt_data ? std::string{} : std::to_string(*opt_data),
               opt_connections, opt_threads_per_connection, rate);
  else
    std::println("{}: {} {} connections, {} threads/connection\n"
                 "    {} RPCs / {} = {} RPCs/sec",
                 server ? server : "INTERNAL",
                 opt_data ? std::format("{} bytes", *opt_data)
                          : std::string("no data"),
                 opt_connections, opt_threads_per_connection, rpc_count.load(),
                 elapsed, formatL(rate));
  if (opt_latency) {
    auto l = *latency.take();
    if (opt_csv)
      std::println(",{},{}", l.mean(), l.stddev());
    else
      std::println("Latency mean {}, stddev {}",
                   duration_cast<microseconds>(l.mean()),
                   duration_cast<microseconds>(l.stddev()));
  }
  else if (opt_csv)
    std::println("");
}

[[noreturn]]
static void
usage(int err)
{
  std::println(
      err ? stderr : stdout,
      "usage: {0} [-C] [-l] [-q] [-m|-c #connections] [-t #threads/conn]\n"
      "       {1:{2}} [-s seconds] [-d bytes] [port [server]]\n"
      "       {0} -S [port]",
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
  while ((opt = getopt(argc, argv, "CSqlmt:c:a:s:d:p:")) != -1)
    switch (opt) {
    case 'C':
    case 'S':
      opt_mode = opt;
      break;
    case 'h':
      usage(0);
      break;
    case 'l':
      opt_latency = true;
      break;
    case 'm':
      opt_multiclient = true;
      break;
    case 'q':
      opt_csv = true;
      break;
    case 't':
      parse_int_opt(opt_threads_per_connection);
      break;
    case 'c':
      parse_int_opt(opt_connections);
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
  if (opt_connections && opt_multiclient) {
    std::println(stderr, "{}: -m and -c are mutually exclusive", progname);
    usage(2);
  }
  if (optind < argc && opt_mode == 'C')
    server = argv[optind++];
  if (optind != argc) {
    std::println(stderr, "{}: too many arguments", progname);
    usage(2);
  }

  if (opt_multiclient)
    opt_connections = llc_map->nbuckets();
  else if (!opt_connections)
    opt_connections = nproc();

  setlinebuf(stdout);
  if (getenv("RPCTRACE"))
    RpcLogger::default_logfd = 1;

  if (opt_mode == 'C') {
    if (!port) {
      std::println(stderr, "Must specify port number with -C");
      exit(2);
    }
    if (opt_multiclient)
      do_client<true>(port, server ? server : "localhost");
    else
      do_client(port, server ? server : "localhost");
  }
  else if (opt_mode == 'S')
    do_server(port, false);
  else if (!opt_mode) {
    std::thread{do_server, 0, true}.detach();
    while (!server_port.load())
      server_port.wait(0);
    if (opt_multiclient)
      do_client<true>(server_port, server ? server : "localhost");
    else
      do_client(server_port, server ? server : "localhost");
  }
  else
    usage(2);

  return 0;
}
