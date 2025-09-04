#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <signal.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
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
#include "x23/format.h"
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

    std::string program_name =
        std::string(CMAKE_ROOT_BINARY_DIR) + "/" +
        std::string(config.program_name.begin(), config.program_name.end());
    std::vector<std::string> program_args =
        std::vector<std::string>(config.program_args.begin(),
                                 config.program_args.end());
    info("starting process: {} {}", x23::show(config.program_name), x23::show(config.program_args));

    // Create pipes for stdout and stderr
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1) {
      error("pipe creation failed: {}", Errno{});
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    // Create temporary files
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path stdout_path = temp_dir / ("hogd_stdout_" + std::to_string(getpid()) + "_XXXXXX");
    std::filesystem::path stderr_path = temp_dir / ("hogd_stderr_" + std::to_string(getpid()) + "_XXXXXX");
    
    // Convert to C-style strings for mkstemp
    std::string stdout_template = stdout_path.string();
    std::string stderr_template = stderr_path.string();
    
    int stdout_fd = mkstemp(stdout_template.data());
    int stderr_fd = mkstemp(stderr_template.data());
    
    if (stdout_fd == -1 || stderr_fd == -1) {
      error("failed to create temporary files: {}", Errno{});
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }
    
    close(stdout_fd);  // We'll reopen in the duplication thread
    close(stderr_fd);
    stdout_temp_path_ = stdout_template;
    stderr_temp_path_ = stderr_template;

    auto pid = fork();
    if (pid < 0) {
      error("fork failed: {}", Errno{});
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    if (pid == 0) {
      // Child process
      
      // Close read ends of pipes
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      
      // Redirect stdout and stderr to write ends of pipes
      if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1 ||
          dup2(stderr_pipe[1], STDERR_FILENO) == -1) {
        // NOLINTBEGIN(modernize-use-std-print)
        fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
        // NOLINTEND(modernize-use-std-print)
        _exit(EXIT_FAILURE);
      }
      
      // Close write ends as they're now duplicated
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);

      std::vector<char *> args;
      args.push_back(const_cast<char *>(config.program_name.data()));

      for (const auto &arg : program_args) {
        args.push_back(const_cast<char *>(arg.c_str()));
      }
      args.push_back(nullptr);

      // NOLINTBEGIN(modernize-use-std-print)
      fprintf(stderr, "execvp: %s %s\n", program_name.c_str(),
              x23::show(config.program_args).c_str());
      execvp(program_name.c_str(), args.data());
      fprintf(stderr, "execvp failed: %s\n", strerror(errno));
      // NOLINTEND(modernize-use-std-print)
      _exit(EXIT_FAILURE);
    }

    // Parent process
    
    // Close write ends of pipes
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    
    // Print temporary file paths
    info("Child process output will be logged to:");
    info("  stdout: {}", stdout_temp_path_);
    info("  stderr: {}", stderr_temp_path_);
    
    // Start threads to duplicate output
    stdout_thread_ = std::jthread([stdout_pipe, stdout_temp_path = stdout_temp_path_]() {
      duplicate_output(stdout_pipe[0], STDOUT_FILENO, stdout_temp_path);
    });
    
    stderr_thread_ = std::jthread([stderr_pipe, stderr_temp_path = stderr_temp_path_]() {
      duplicate_output(stderr_pipe[0], STDERR_FILENO, stderr_temp_path);
    });

    // Start thread to wait for child process
    waitpid_thread_ = std::jthread([pid]() {
      wait_for_child(pid);
    });

    info("forked process: {}", pid);
    running_pid_ = pid;

    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  void ciserver_stop()
  {

    if (running_pid_) {
      info("stopping running process: {}", *running_pid_);
      kill(*running_pid_, SIGTERM);
      
      // Wait for output duplication threads to finish
      if (stdout_thread_.joinable()) {
        stdout_thread_.join();
      }
      if (stderr_thread_.joinable()) {
        stderr_thread_.join();
      }
      
      // Wait for waitpid thread to finish
      if (waitpid_thread_.joinable()) {
        waitpid_thread_.join();
      }
      
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

private:
  // Helper function to duplicate output from pipe to both console and temp file
  static void duplicate_output(int pipe_fd, int console_fd, const std::string& temp_path) {
    std::ofstream temp_file(temp_path);
    if (!temp_file.is_open()) {
      error("Failed to open temp file: {}", temp_path);
      return;
    }
    
    char buffer[4096];
    ssize_t bytes_read;
    
    while ((bytes_read = read(pipe_fd, buffer, sizeof(buffer))) > 0) {
      // Write to console (original stdout/stderr)
      write(console_fd, buffer, bytes_read);
      
      // Write to temp file
      temp_file.write(buffer, bytes_read);
      temp_file.flush();
    }
    
    temp_file.close();
    close(pipe_fd);
  }

  // Helper function to wait for child process and log exit status
  static void wait_for_child(pid_t child_pid) {
    int status;
    pid_t result = waitpid(child_pid, &status, 0);
    
    if (result == -1) {
      error("waitpid failed for child {}: {}", child_pid, Errno{});
      return;
    }
    
    if (WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if (exit_code == 0) {
        info("Child process {} exited successfully with code {}", child_pid, exit_code);
      } else {
        error("Child process {} exited with error code {}", child_pid, exit_code);
      }
    } else if (WIFSIGNALED(status)) {
      int signal_num = WTERMSIG(status);
      error("Child process {} terminated by signal {} ({})", child_pid, signal_num, strsignal(signal_num));
    } else {
      error("Child process {} terminated with unknown status {}", child_pid, status);
    }
  }

public:

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
    
    // Ensure output duplication threads are stopped
    if (stdout_thread_.joinable()) {
      stdout_thread_.request_stop();
      stdout_thread_.join();
    }
    if (stderr_thread_.joinable()) {
      stderr_thread_.request_stop();
      stderr_thread_.join();
    }
    
    // Ensure waitpid thread is stopped
    if (waitpid_thread_.joinable()) {
      waitpid_thread_.request_stop();
      waitpid_thread_.join();
    }
  }

private:
  uint16_t port_number_ = 0;
  std::jthread listen_thread_;
  x23::UniqueFd lfd_;

  std::atomic<bool> waiting_for_shutdown_ = false;

  std::optional<uint32_t> running_pid_;
  
  // For output duplication
  std::jthread stdout_thread_;
  std::jthread stderr_thread_;
  std::string stdout_temp_path_;
  std::string stderr_temp_path_;
  
  // For waiting on child process
  std::jthread waitpid_thread_;
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
