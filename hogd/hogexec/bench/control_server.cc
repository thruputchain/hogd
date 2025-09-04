#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ci_server.hh"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/init.h"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

class CIControlServer {
public:
  using rpc_service = CIServer1;

  CIServerErrorCode ciserver_start(ServerConfig const &config)
  {
    if (running_pid_) {
      error("already running process: {}", *running_pid_);
      return CIServerErrorCode::kCIServerErrorCodeAlreadyRunning;
    }
    info("starting process: {} {}", config.program_name, config.program_args);

    auto pid = fork();
    if (pid < 0) {
      error("fork failed: {}", Errno{});
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    if (pid == 0) {
      std::string program_name =
          std::string(config.program_name.begin(), config.program_name.end());
      std::vector<std::string> program_args =
          std::vector<std::string>(config.program_args.begin(),
                                   config.program_args.end());

      // Child process
      std::vector<char *> args;
      args.push_back(const_cast<char *>(config.program_name.data()));

      for (const auto &arg : program_args) {
        args.push_back(const_cast<char *>(arg.c_str()));
      }
      args.push_back(nullptr);
      info("execvp: {} {}", program_name, args);
      execvp(program_name.c_str(), args.data());

      // If execvp returns, it failed
      error("execvp failed: {}", Errno{});
      exit(EXIT_FAILURE);
    }
    
    info("forked process: {}", pid);

    // Parent process
    running_pid_ = pid;

    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  void ciserver_stop()
  {

    if (running_pid_) {
      info("stopping running process: {}", *running_pid_);
      kill(*running_pid_, SIGTERM);
      running_pid_.reset();
    }
    else {
      error("no running process to stop");
    }

    running_pid_.reset();
  }

  void ciserver_shutdown()
  {
    info("shutting down control server");
    waiting_for_shutdown_ = true;
    waiting_for_shutdown_.notify_all();
  }

  void wait_for_shutdown() { waiting_for_shutdown_.wait(false); }

  void start_server(uint32_t port)
  {
    lfd_ = x23::stcplisten(port);
    if (!lfd_) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    if (port == 0) {
      throw std::runtime_error("port cannot be 0 for CI control server");
    }

    port_number_ = x23::get_port(x23::local_addr(lfd_.get()));

    listen_thread_ = std::jthread([this](std::stop_token &&stop) {
      while (!stop.stop_requested()) {

        sockaddr_storage ss;
        socklen_t sslen = sizeof(ss);
        x23::UniqueFd fd{
            accept(lfd_.get(), reinterpret_cast<sockaddr *>(&ss), &sslen)};
        if (!fd) {
          info("accept error: {}", Errno{});
        }
        else {
          x23::srpc_stream_logged(fd.release())->server(this);
        }
      }
    });
  }

  ~CIControlServer()
  {
    listen_thread_.request_stop();
    shutdown(lfd_.get(), SHUT_RDWR);
    lfd_.reset();
  }

private:
  uint16_t port_number_ = 0;
  std::jthread listen_thread_;
  x23::UniqueFd lfd_;

  std::atomic<bool> waiting_for_shutdown_ = false;

  std::optional<uint32_t> running_pid_;
};

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -p <control_port>", argv[0]);
  exit(EXIT_FAILURE);
}
} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::optional<int> control_port;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "p:")) != -1) {
    switch (opt) {
    case 'p':
      control_port = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!control_port) {
    control_port = hogd::kControlServerPort;
  }

  hogd::CIControlServer control_server;

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  control_server.start_server(*control_port);
  control_server.wait_for_shutdown();
  return 0;
}
