#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <print>
#include <string.h> // NOLINT(modernize-deprecated-headers)
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ci_server.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "x23/format.h"

namespace hogd {

class CIControlRpc {
public:
  using rpc_service = CIServer1;

  ~CIControlRpc()
  {
    // Ensure waitpid thread is stopped
    if (waitpid_thread_.joinable()) {
      waitpid_thread_.request_stop();
      waitpid_thread_.join();
    }
  }

  CIServerErrorCode ciserver_start(HogdGitHash const &hogd_git_hash,
                                   ServerConfig const &config)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}", hogd_git_hash,
            version::kHogdGitHash);
      return CIServerErrorCode::kCIServerErrorCodeVersionMismatch;
    }

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
    info("starting process: {} {}", x23::show(config.program_name),
         x23::show(config.program_args));

    // Create temporary files
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path stdout_path =
        temp_dir / ("hogd_stdout_" + std::to_string(getpid()) + "_XXXXXX");
    std::filesystem::path stderr_path =
        temp_dir / ("hogd_stderr_" + std::to_string(getpid()) + "_XXXXXX");

    // Convert to C-style strings for mkstemp
    std::string stdout_template = stdout_path.string();
    std::string stderr_template = stderr_path.string();

    // NOLINTBEGIN(misc-include-cleaner)
    int stdout_fd = mkstemp(stdout_template.data());
    int stderr_fd = mkstemp(stderr_template.data());
    // NOLINTEND(misc-include-cleaner)

    if (stdout_fd == -1 || stderr_fd == -1) {
      error("failed to create temporary files: {}", Errno{});
      if (stdout_fd != -1)
        close(stdout_fd);
      if (stderr_fd != -1)
        close(stderr_fd);
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    stdout_temp_path_ = stdout_template;
    stderr_temp_path_ = stderr_template;

    // Save the parent's original stdout/stderr before fork
    int parent_stdout = dup(STDOUT_FILENO);
    if (parent_stdout == -1) {
      error("failed to save parent stdout: {}", Errno{});
      close(stdout_fd);
      close(stderr_fd);
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    int parent_stderr = dup(STDERR_FILENO);
    if (parent_stderr == -1) {
      error("failed to save parent stderr: {}", Errno{});
      close(stdout_fd);
      close(stderr_fd);
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    // Print temporary file paths BEFORE fork to ensure parent logging works
    info("Child process output will be logged to:");
    info("  stdout: {}", stdout_temp_path_);
    info("  stderr: {}", stderr_temp_path_);

    auto pid = fork();
    if (pid < 0) {
      error("fork failed: {}", Errno{});
      close(parent_stderr);
      close(parent_stdout);
      close(stdout_fd);
      close(stderr_fd);
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    if (pid == 0) {
      // Child process

      // Redirect stdout and stderr to temp files
      if (dup2(stdout_fd, STDOUT_FILENO) == -1 ||
          dup2(stderr_fd, STDERR_FILENO) == -1) {
        // NOLINTBEGIN(modernize-use-std-print)
        fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
        // NOLINTEND(modernize-use-std-print)
        _exit(EXIT_FAILURE);
      }

      // Close the original file descriptors
      close(stdout_fd);
      close(stderr_fd);

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

    // Restore parent's original stderr in case child affected it
    if (dup2(parent_stderr, STDERR_FILENO) == -1) {
      error("failed to restore parent stderr: {}", Errno{});
    }
    close(parent_stderr);

    // Restore parent's original stdout in case child affected it
    if (dup2(parent_stdout, STDOUT_FILENO) == -1) {
      error("failed to restore parent stdout: {}", Errno{});
    }
    close(parent_stdout);

    // Close the file descriptors in parent (they're only needed in child)
    close(stdout_fd);
    close(stderr_fd);

    std::lock_guard<std::mutex> lock(running_pid_mutex_);
    running_pid_ = pid;
    // Start thread to wait for child process
    waitpid_thread_ = std::jthread([pid, this]() { wait_for_child(pid); });

    info("forked process: {}", pid);

    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  void ciserver_stop(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}", hogd_git_hash,
            version::kHogdGitHash);
      return;
    }

    auto pid = [&]() {
      std::lock_guard<std::mutex> lock(running_pid_mutex_);
      return running_pid_;
    }();

    if (pid) {
      info("stopping running process: {}", *pid);
      kill(*pid, SIGTERM); // NOLINT(misc-include-cleaner)

      // Wait for waitpid thread to finish
      if (waitpid_thread_.joinable()) {
        waitpid_thread_.join();
      }
    }
    else {
      error("no running process to stop");
    }
  }

  void ciserver_shutdown(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}", hogd_git_hash,
            version::kHogdGitHash);
      return;
    }

    info("stopping any running process");
    ciserver_stop(hogd_git_hash);

    info("shutting down control server");
    waiting_for_shutdown_ = true;
    waiting_for_shutdown_.notify_all();
  }

  CIProcStatus ciserver_query_proc_status(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}", hogd_git_hash,
            version::kHogdGitHash);
      return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeVersionMismatch);
    }

    std::lock_guard<std::mutex> lock(running_pid_mutex_);
    if (!running_pid_) {

      if (most_recent_exit_code_.has_value()) {
        CIProcStatus status(CIServerErrorCode::kCIServerErrorCodeOK);
        status.exit_code() = *most_recent_exit_code_;
        return status;
      }
      error("queried proc status before starting a process");
      return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeInternal);
    }
    return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeAlreadyRunning);
  }

  void wait_for_shutdown() { waiting_for_shutdown_.wait(false); }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

  void await_shutdown_request()
  {
    // TODO(jhumphri): Implement.
  }

private:
  static bool check_hogd_git_hash(HogdGitHash const &hogd_git_hash)
  {
    return std::string(hogd_git_hash) == version::kHogdGitHash;
  }

  // Helper function to wait for child process and log exit status
  // NOLINTBEGIN(misc-include-cleaner)
  void wait_for_child(pid_t child_pid)
  {
    int status;
    auto result = waitpid(child_pid, &status, 0);

    if (result == -1) {
      error("waitpid failed for child {}: {}", child_pid, Errno{});
      return;
    }

    int exit_code = 0;

    if (WIFEXITED(status)) {
      exit_code = WEXITSTATUS(status);
      if (exit_code == 0) {
        info("Child process {} exited successfully with code {}", child_pid,
             exit_code);
      }
      else {
        error("Child process {} exited with error code {}", child_pid,
              exit_code);
      }
    }
    else if (WIFSIGNALED(status)) {
      int signal_num = WTERMSIG(status);
      error("Child process {} terminated by signal {} ({})", child_pid,
            signal_num, strsignal(signal_num));
      // chatgpt says this matches the behavior of most shells
      exit_code = 128 + signal_num;
    }
    else {
      error("Child process {} terminated with unknown status {}", child_pid,
            status);
      exit_code = -1;
    }

    std::lock_guard<std::mutex> lock(running_pid_mutex_);
    running_pid_.reset();
    most_recent_exit_code_ = exit_code;
  }
  // NOLINTEND(misc-include-cleaner)

  std::atomic<bool> waiting_for_shutdown_ = false;

  std::mutex running_pid_mutex_;
  std::optional<uint32_t> running_pid_;
  std::optional<int> most_recent_exit_code_ = std::nullopt;

  // For output logging
  std::string stdout_temp_path_;
  std::string stderr_temp_path_;

  // For waiting on child process
  std::jthread waitpid_thread_;
};

class CIControlServer : public RpcServer<CIControlRpc> {};

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
  control_server.impl_.wait_for_shutdown();
  return 0;
}
