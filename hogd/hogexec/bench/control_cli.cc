#include <cstdio>
#include <cstdlib>
#include <exception>
#include <print>
#include <string>

#include "control_client.h"
#include "hogd/init.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace {

void
usage(char *argv[])
{
  std::println("Usage: {} <command> <host> <port>", argv[0]);
  std::println("Commands:");
  std::println("  shutdown  - Shutdown the control server");
  exit(EXIT_FAILURE);
}

int
handle_shutdown_command(std::string const &host, std::string const &port)
{
  try {
    x23::ThreadPool pool;
    x23::CoroPoll cp(pool);
    auto client = hogd::connect_to_control_server(cp, host, port);

    std::println("Sending shutdown command to {}:{}", host, port);
    client.shutdown_server();
    std::println("Shutdown command sent successfully");

    return 0;
  } catch (std::exception const &e) {
    std::println(stderr, "Error: {}", e.what());
    return 1;
  }
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  if (argc != 4) {
    usage(argv);
  }

  std::string command = argv[1];
  std::string host = argv[2];
  std::string port = argv[3];

  if (command == "shutdown") {
    return handle_shutdown_command(host, port);
  }

  std::println(stderr, "Unknown command: {}", command);
  usage(argv);

  return 0;
}
