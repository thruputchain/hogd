#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <getopt.h>
#include <print>
#include <string>
#include <unistd.h>

#include "hogd/hogexec/hogexec_core.h"
#include "hogd/init.h"
#include "rpc_types.hh"

namespace hogd {

namespace {
std::string progname = "hogexec";

[[noreturn]] void
usage(int err = STDERR_FILENO)
{
  std::print(err == 0 ? stdout : stderr,
             "Usage: {} \n"
             "-s --exec_server_port <port>\n"
             "-c --coord_server_address <address>\n"
             "-p --coord_server_port <port>\n"
             "-n --ncorothreads <num> (default: 1+nworkthreads/4)\n"
             "-m --nworkthreads <num\n",
             progname);
  exit(err);
}

int
hogexec_main(int argc, char **argv)
{
  if (argc > 0)
    progname = std::filesystem::path(argv[0]).filename();

  struct option long_options[] = {
      {.name = "help", .has_arg = no_argument, .flag = nullptr, .val = 'h'},
      {.name = "exec_server_port",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 's'},
      {.name = "coord_address",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'c'},
      {.name = "coord_port",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'p'},
      {.name = "ncorothreads",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'n'},
      {.name = "nworkthreads",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'm'},
      {.name = nullptr, .has_arg = 0, .flag = nullptr, .val = 0}};

  int long_index = 0;
  int opt;
  const char *shortopts = "hs:c:p:n:m:";

  // parameters
  std::string exec_server_port = std::to_string(kExecToCoordPortNum);
  std::string coord_address = "localhost";
  std::string coord_port = std::to_string(kCoordAnnouncePortNum);

  unsigned int nthreads = 0;
  unsigned int nworkthreads = 1;

  // NOLINTNEXTLINE(misc-include-cleaner)
  while ((opt = getopt_long(argc, argv, shortopts, long_options,
                            &long_index)) != -1) {
    switch (opt) {
    case 'h':
      usage(0);
      return EXIT_SUCCESS;
    case 's':
      // NOLINTNEXTLINE(misc-include-cleaner)
      exec_server_port = optarg;
      break;
    case 'c':
      // NOLINTNEXTLINE(misc-include-cleaner)
      coord_address = optarg;
      break;
    case 'p':
      // NOLINTNEXTLINE(misc-include-cleaner)
      coord_port = optarg;
      break;
    case 'n':
      // NOLINTNEXTLINE(misc-include-cleaner)
      nthreads = atoi(optarg);
      break;
    case 'm':
      // NOLINTNEXTLINE(misc-include-cleaner)
      nworkthreads = atoi(optarg);
      break;
    default:
      usage(0);
      return EXIT_FAILURE;
    }
  }

  if (nthreads == 0) {
    nthreads = (nworkthreads / 4) + 1;
  }

  if (exec_server_port.empty() || coord_address.empty() || coord_port.empty()) {
    usage(0);
    return EXIT_FAILURE;
  }

  HogexecCore core = start_hogexec(nthreads, nworkthreads, exec_server_port,
                                   coord_address, coord_port);

  core.announce_ready_to_coord(coord_address, coord_port);
  core.await_shutdown();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  return hogd::hogexec_main(argc, argv);
}
