#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <getopt.h>
#include <print>
#include <string>
#include <unistd.h>

#include "hogd/hogdb/hogdb_core.h"
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
             "-e --to_exec_server_port <port>\n"
             "-c --to_coord_server_port <port>\n"
             "-a --coord_server_address <address>\n"
             "-p --coord_server_port <port>\n"
             "-n --nthreads <num>\n",
             progname);
  exit(err);
}

int
hogdb_main(int argc, char **argv)
{
  if (argc > 0)
    progname = std::filesystem::path(argv[0]).filename();

  struct option long_options[] = {
      {.name = "help", .has_arg = no_argument, .flag = nullptr, .val = 'h'},
      {.name = "to_exec_server_port",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'e'},
      {.name = "to_coord_server_port",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'c'},
      {.name = "coord_address",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'a'},
      {.name = "coord_port",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'p'},
      {.name = "nthreads",
       .has_arg = required_argument,
       .flag = nullptr,
       .val = 'n'},
      {.name = nullptr, .has_arg = 0, .flag = nullptr, .val = 0}};

  int long_index = 0;
  int opt;
  const char *shortopts = "he:c:a:p:n:";

  // parameters
  std::string to_coord_server_port = std::to_string(kDBToCoordPortNum);
  std::string to_exec_server_port = std::to_string(kDBToExecutorPortNum);
  std::string coord_address = "localhost";
  std::string coord_port = std::to_string(kCoordAnnouncePortNum);

  unsigned int nthreads = 1;

  // NOLINTNEXTLINE(misc-include-cleaner)
  while ((opt = getopt_long(argc, argv, shortopts, long_options,
                            &long_index)) != -1) {
    switch (opt) {
    case 'h':
      usage(0);
      return EXIT_SUCCESS;
    case 'e':
      // NOLINTNEXTLINE(misc-include-cleaner)
      to_exec_server_port = optarg;
      break;
    case 'c':
      // NOLINTNEXTLINE(misc-include-cleaner)
      to_coord_server_port = optarg;
      break;
    case 'a':
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
    default:
      usage(0);
      return EXIT_FAILURE;
    }
  }

  if (to_exec_server_port.empty() || to_coord_server_port.empty() ||
      coord_address.empty() || coord_port.empty()) {
    usage(0);
    return EXIT_FAILURE;
  }

  HogdbCore db(nthreads, to_exec_server_port, to_coord_server_port);

  // TODO(geoff): load from disk, resolve initialization
  db.get_root_db().set_as_default_genesis();
  db.announce_to_coordinator(coord_address, coord_port);

  db.await_shutdown();

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
  return hogd::hogdb_main(argc, argv);
}
