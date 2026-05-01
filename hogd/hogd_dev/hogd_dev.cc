#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <stdexcept>
#include <string>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/hogd_dev/hogd_dev_node.h"
#include "hogd/init.h"

namespace hogd {

namespace {

enum class ReadinessSink { kStdout };

// Darwin's POSIX signal declarations are not fully modeled by include-cleaner.
// NOLINTBEGIN(misc-include-cleaner)
volatile sig_atomic_t shutdown_requested =
    0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

struct ShutdownSignals {
  sigset_t suspend_mask;
};

void
request_shutdown(int signal_number)
{
  (void)signal_number;
  shutdown_requested = 1;
}

ShutdownSignals
block_shutdown_signal()
{
  sigset_t blocked_mask;
  CHECK_EQ(sigemptyset(&blocked_mask), 0);
  CHECK_EQ(sigaddset(&blocked_mask, SIGTERM), 0);

  sigset_t suspend_mask;
  CHECK_EQ(sigprocmask(SIG_BLOCK, &blocked_mask, &suspend_mask), 0);
  CHECK_EQ(sigdelset(&suspend_mask, SIGTERM), 0);

  return {.suspend_mask = suspend_mask};
}

void
install_signal_handler()
{
  struct sigaction action{};
  action.sa_handler = request_shutdown;
  CHECK_EQ(sigemptyset(&action.sa_mask), 0);
  CHECK_EQ(sigaction(SIGTERM, &action, nullptr), 0);
}

void
wait_for_shutdown(const ShutdownSignals &signals)
{
  while (!shutdown_requested) {
    // SIGTERM is blocked outside sigsuspend(), so checking the flag and
    // sleeping are atomic with respect to the shutdown signal.
    sigsuspend(&signals.suspend_mask);
  }
  info("hogd-dev shutdown requested");
  std::fflush(stderr);
}
// NOLINTEND(misc-include-cleaner)

void
emit_readiness(const HogdDevNode &node, ReadinessSink sink)
{
  switch (sink) {
  case ReadinessSink::kStdout:
    std::println("{{\"status\":\"ready\",\"network_id\":\"groundhog-local\","
                 "\"endpoints\":{{\"scheduler\":\"{}\"}}}}",
                 format_advertised_address(node.bind_host(),
                                           node.scheduler_port()));
    std::fflush(stdout);
  }
}

int
hogd_dev_init(std::string listen_host, std::uint16_t requested_scheduler_port,
              ReadinessSink readiness_sink)
{
  install_signal_handler();

  HogdDevNode node(std::move(listen_host), requested_scheduler_port);
  emit_readiness(node, readiness_sink);
  info("hogd-dev initialization complete.");

  ShutdownSignals shutdown_signals = block_shutdown_signal();
  wait_for_shutdown(shutdown_signals);
  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println(stderr,
               "usage: {} [--listen-host <ip>] [--scheduler-port <port>] "
               "[--readiness stdout]",
               argv[0]);
  exit(EXIT_FAILURE);
}

std::uint16_t
parse_port(const std::string &value)
{
  std::size_t pos = 0;
  std::uint64_t port = std::stoul(value, &pos, 10);
  if (pos != value.size() || port > 65535) {
    throw std::runtime_error(std::format("Invalid port: {}", value));
  }
  return static_cast<std::uint16_t>(port);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogd-dev!");

  std::string listen_host = "127.0.0.1";
  std::uint16_t scheduler_port = 0;
  hogd::ReadinessSink readiness_sink = hogd::ReadinessSink::kStdout;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--listen-host" && i + 1 < argc) {
      listen_host = argv[++i];
      continue;
    }
    if (arg == "--scheduler-port" && i + 1 < argc) {
      scheduler_port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--readiness" && i + 1 < argc) {
      const std::string readiness = argv[++i];
      if (readiness != "stdout") {
        usage(argv);
      }
      readiness_sink = hogd::ReadinessSink::kStdout;
      continue;
    }
    usage(argv);
  }

  return hogd::hogd_dev_init(std::move(listen_host), scheduler_port,
                             readiness_sink);
}
