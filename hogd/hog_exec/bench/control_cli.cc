#include <chrono>
#include <cstdlib>
#include <exception>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ci_server.hh"
#include "control_client.h"
#include "hogd/base/log.h"
#include "hogd/init.h"

namespace {

void
usage(char *argv[])
{
  std::println("Usage: {} <host> <port> <command> <args...>", argv[0]);
  std::println("Commands:");
  std::println("  shutdown        - Shutdown the control server");
  std::println("  start [--cores <cpu-list>] <program> <args...>");
  std::println("  stop            - Stop a process");
  std::println("  wait            - Wait for the most recent process to exit");
  std::println("  expect_running  - Check if a process is running");
  std::println("  clear_work_dir  - Clear the work directory");
  std::println("  ready           - Check if this build's server is serving");
  exit(EXIT_FAILURE);
}

// Reports whether the server answering on [host]:[port] is a control server
// built from this commit.
//
// A control server left over from an earlier run answers a plain TCP connect
// exactly as readily as this run's does, so a readiness wait built on [nc -z]
// alone lets a benchmark proceed against the stale server and only discover
// the mismatch when the first real RPC comes back
// [kCIServerErrorCodeVersionMismatch] -- which the benchmarks treat as fatal.
// See hogd issue #3940.
//
// [query_work_dir] is the cheapest read-only RPC that runs the server's
// version check, so it doubles as the probe: it mutates nothing on a server
// that turns out to be the right one.
int
handle_ready_command(std::string const &host, std::string const &port)
{
  try {
    // No retry: the caller polls, so a host whose server has not come up yet
    // has to be reported as not-ready promptly rather than block in connect.
    auto client = hogd::connect_to_control_server(host, port, false);
    auto status = client.query_work_dir();
    if (status.error_code() == hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
      hogd::info("Control server on {}:{} is serving this build", host, port);
      return 0;
    }
    hogd::info("Control server on {}:{} is not ready: {}", host, port,
               status.error_code());
    return 1;
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
    return 1;
  }
}

int
handle_expect_running_command(std::string const &host, std::string const &port)
{
  try {
    auto client = hogd::connect_to_control_server(host, port);
    auto status = client.query_proc_status();
    if (status.error_code() ==
        hogd::CIServerErrorCode::kCIServerErrorCodeAlreadyRunning) {
      hogd::info("Process is running");
      return 0;
    }
    if (status.error_code() == hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
      hogd::info("Process is not running: exit code {}", status.exit_code());
      return 1;
    }
    hogd::info("Error: {}", status.error_code());
    return 1;
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
    return 1;
  }
}

int
handle_shutdown_command(std::string const &host, std::string const &port)
{
  try {
    // This command does not retry on failure to connect;
    // in that case, the control server is already not running.
    // This is relevant in case of crash in CI.
    auto client = hogd::connect_to_control_server(host, port, false);

    hogd::info("Sending shutdown command to {}:{}", host, port);
    client.shutdown_server();
    hogd::info("Shutdown command sent successfully");

    return 0;
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
    return 1;
  }
}

int
handle_start_command(std::string const &host, std::string const &port,
                     std::string const &cpu_affinity,
                     std::vector<std::string> const &args)
{
  try {
    if (args.empty()) {
      hogd::info("Error: start requires a program name");
      return 1;
    }
    auto client = hogd::connect_to_control_server(host, port);
    // gcc14 has vector<string> format problems in CI
    std::string command;
    if (!cpu_affinity.empty()) {
      command += "[cores=" + cpu_affinity + "] ";
    }
    for (auto const &arg : args) {
      command += arg + " ";
    }
    hogd::info("Starting server: {}", command);
    auto status = client.start_server(args[0],
                                      std::vector<std::string>(args.begin() + 1,
                                                               args.end()),
                                      cpu_affinity);
    hogd::info("Start command sent successfully");
    std::println("CONTROL_STDOUT_PATH={}",
                 std::string(status.paths().stdout_path.begin(),
                             status.paths().stdout_path.end()));
    std::println("CONTROL_STDERR_PATH={}",
                 std::string(status.paths().stderr_path.begin(),
                             status.paths().stderr_path.end()));
    return 0;
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
    return 1;
  }
}

int
handle_stop_command(std::string const &host, std::string const &port)
{
  try {
    auto client = hogd::connect_to_control_server(host, port, false);
    client.stop_server();
    return 0;
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
    return 1;
  }
}

int
handle_clear_work_dir_command(std::string const &host, std::string const &port)
{
  try {
    auto client = hogd::connect_to_control_server(host, port);
    client.clear_work_dir();
    return 0;
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
    return 1;
  }
}

int
handle_wait_command(std::string const &host, std::string const &port)
{
  try {
    auto client = hogd::connect_to_control_server(host, port);
    while (true) {
      auto status = client.query_proc_status();
      if (status.error_code() ==
          hogd::CIServerErrorCode::kCIServerErrorCodeAlreadyRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
      if (status.error_code() ==
          hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
        hogd::info("Process exited with code {}", status.exit_code());
        return status.exit_code();
      }
      hogd::info("Error: {}", status.error_code());
      return 1;
    }
  } catch (std::exception const &e) {
    hogd::info("Error: {}", e.what());
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

  if (argc < 4) {
    usage(argv);
  }

  std::string host = argv[1];
  std::string port = argv[2];
  std::string command = argv[3];

  if (command == "shutdown") {
    return handle_shutdown_command(host, port);
  }
  if (command == "start") {
    std::string cpu_affinity;
    int args_start = 4;
    if (argc > 4 && std::string_view(argv[4]) == "--cores") {
      if (argc < 7) {
        usage(argv);
      }
      cpu_affinity = argv[5];
      args_start = 6;
    }
    return handle_start_command(host, port, cpu_affinity,
                                std::vector<std::string>(argv + args_start,
                                                         argv + argc));
  }
  if (command == "stop") {
    return handle_stop_command(host, port);
  }
  if (command == "wait") {
    return handle_wait_command(host, port);
  }
  if (command == "expect_running") {
    return handle_expect_running_command(host, port);
  }

  if (command == "clear_work_dir") {
    return handle_clear_work_dir_command(host, port);
  }

  if (command == "ready") {
    return handle_ready_command(host, port);
  }

  hogd::info("Unknown command: {}", command);
  usage(argv);

  return 0;
}
