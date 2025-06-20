
#include <charconv>

#include <x23/sockio.h>
#include <x23/srpc.h>
#include <x23/threadpool.h> // for nproc()

#include "tests/xdrtest.hh"

using namespace x23;

std::string progname;
bool opt_csv;
unsigned opt_connections;
unsigned opt_seconds = 1;
unsigned opt_threads_per_connection = 1;
std::optional<std::uint32_t> opt_data;

std::atomic<std::uint16_t> server_port{0};

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

Server server;

void
do_server(std::uint16_t port, bool quiet)
{
  UniqueFd lfd = tcplisten(port, opt_connections + 5);
  server_port = get_port(local_addr(lfd));
  server_port.notify_all();
  if (port == 0 && !quiet)
    std::println("port {}", server_port.load());

  for (;;) {
    sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    UniqueFd fd {accept(lfd.get(), reinterpret_cast<sockaddr *>(&ss), &sslen)};
    std::println("accepted fd {} TCP port {} from {}", fd.get(),
		 server_port.load(), HostPort(ss));
  }

  std::weak_ptr<int> x;

  exit(1);
}

void
do_client(int port, const char *server)
{}

[[noreturn]]
static void
usage(int err)
{
  std::println(err ? stderr : stdout,
               "usage: {} [-C|-S] [-q] [-c #connections] [-t #threads/conn]\n"
               "       {:{}}  [-s seconds] [-d bytes] [port [server]]\n",
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
  if (optind < argc && opt_mode == 'C')
    server = argv[optind++];
  if (optind != argc) {
    std::println(stderr, "{}: too many arguments", progname);
    usage(2);
  }

  if (!opt_connections)
    opt_connections = nproc();

  setlinebuf(stdout);
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
