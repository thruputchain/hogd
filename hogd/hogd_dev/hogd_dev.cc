#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <print>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/string.h"
#include "hogd/hogd_dev/dev_account_bootstrap.h"
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

template<std::size_t N>
std::string
format_hex(std::array<std::uint8_t, N> const &bytes)
{
  return std::format("0x{}", encode_hex(std::as_bytes(std::span(bytes))));
}

std::string
build_readiness_payload(const HogdDevNode &node)
{
  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("status", "ready");
    w.field("network_id", "groundhog-local");
    w.object_field("endpoints", [&](json::Writer &w) {
      w.field("scheduler", format_advertised_address(node.bind_host(),
                                                     node.scheduler_port()));
      w.field("read",
              format_advertised_address(node.bind_host(), node.read_port()));
      w.field("gateway",
              format_advertised_address(node.bind_host(), node.gateway_port()));
    });
    if (!node.dev_accounts().empty()) {
      w.array_field("dev_accounts", [&](json::Writer &w) {
        for (DevAccountMetadata const &account : node.dev_accounts()) {
          CHECK_LE(account.storage_tokens,
                   static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()));
          w.object([&](json::Writer &w) {
            w.field("index", static_cast<std::int64_t>(account.index));
            w.field("address", format_hex(account.address));
            w.field("public_key", format_hex(account.public_key));
            w.field("initial_storage_tokens",
                    static_cast<std::int64_t>(account.storage_tokens));
          });
        }
      });
    }
  });
  return buffer;
}

void
emit_readiness(const HogdDevNode &node, ReadinessSink sink)
{
  switch (sink) {
  case ReadinessSink::kStdout:
    std::println("{}", build_readiness_payload(node));
    std::fflush(stdout);
  }
}

int
hogd_dev_init(std::string listen_host, std::uint16_t requested_scheduler_port,
              ReadinessSink readiness_sink,
              DevAccountBootstrapConfig const &bootstrap_config)
{
  install_signal_handler();

  HogdDevNode node(std::move(listen_host), requested_scheduler_port,
                   std::move(bootstrap_config));
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
               "[--readiness stdout] [--dev-accounts <count>] "
               "[--dev-account-seed <seed>] "
               "[--dev-account-tokens <tokens>]",
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

std::uint32_t
parse_u32(const std::string &value, std::string_view name)
{
  std::size_t pos = 0;
  std::uint64_t parsed = std::stoull(value, &pos, 10);
  if (pos != value.size() ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  return static_cast<std::uint32_t>(parsed);
}

std::uint32_t
parse_dev_account_count(const std::string &value)
{
  std::uint32_t count = parse_u32(value, "dev account count");
  if (count > hogd::kMaxDevAccounts) {
    throw std::runtime_error(
        std::format("Invalid dev account count: {} exceeds maximum {}", value,
                    hogd::kMaxDevAccounts));
  }
  return count;
}

std::uint64_t
parse_u64(const std::string &value, std::string_view name)
{
  std::size_t pos = 0;
  std::uint64_t parsed = std::stoull(value, &pos, 10);
  if (pos != value.size() ||
      parsed > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error(std::format("Invalid {}: {}", name, value));
  }
  return parsed;
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
  hogd::DevAccountBootstrapConfig bootstrap_config;

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
    if (arg == "--dev-accounts" && i + 1 < argc) {
      bootstrap_config.count = parse_dev_account_count(argv[++i]);
      continue;
    }
    if (arg == "--dev-account-seed" && i + 1 < argc) {
      bootstrap_config.seed = argv[++i];
      continue;
    }
    if (arg == "--dev-account-tokens" && i + 1 < argc) {
      bootstrap_config.storage_tokens =
          parse_u64(argv[++i], "dev account tokens");
      continue;
    }
    usage(argv);
  }

  return hogd::hogd_dev_init(std::move(listen_host), scheduler_port,
                             readiness_sink, bootstrap_config);
}
