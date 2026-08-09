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
  exit(EXIT_FAILURE);
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
    auto error_code = client.shutdown_server();
    if (error_code != hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
      hogd::info("Error: shutdown of {}:{} reported {}", host, port,
                 error_code);
      return 1;
    }
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

// [stop], [shutdown] and [clear_work_dir] exist to leave the host in a known
// state, and the CI scripts branch on their exit status. Reporting success for
// a request the server refused -- a version mismatch, a work directory it
// could not remove, a benchmark process it could not kill -- hands the caller
// a clear host that is not clear, and the failure resurfaces later as a bind
// or readiness failure somewhere else. Give back what the server said.
int
handle_stop_command(std::string const &host, std::string const &port)
{
  try {
    auto client = hogd::connect_to_control_server(host, port, false);
    auto error_code = client.stop_server();
    if (error_code != hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
      hogd::info("Error: stop on {}:{} reported {}", host, port, error_code);
      return 1;
    }
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
    auto error_code = client.clear_work_dir();
    if (error_code != hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
      hogd::info("Error: clear_work_dir on {}:{} reported {}", host, port,
                 error_code);
      return 1;
    }
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

  hogd::info("Unknown command: {}", command);
  usage(argv);

  return 0;
}
