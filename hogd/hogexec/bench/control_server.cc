#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string.h> // NOLINT(modernize-deprecated-headers)
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/status.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "x23/config.h"
#include "x23/cpumap.h"
#include "x23/fastclock.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// NOLINTBEGIN(misc-include-cleaner)
std::optional<int>
wait_for_child(pid_t child_pid)
{
  int status;
  auto result = waitpid(child_pid, &status, 0);

  if (result == -1) {
    error("waitpid failed for child {}: {}", child_pid, Errno{});
    return std::nullopt;
  }

  int exit_code = 0;

  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
    if (exit_code == 0) {
      info("Child process {} exited successfully with code {}", child_pid,
           exit_code);
    }
    else {
      error("Child process {} exited with error code {}", child_pid, exit_code);
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

  return exit_code;
}
// NOLINTEND(misc-include-cleaner)

std::string
join_args(std::vector<std::string> const &args)
{
  if (args.empty()) {
    return "";
  }
  std::string args_joined;
  for (const auto &arg : args) {
    args_joined += "\"" + arg + "\" ";
  }
  args_joined.pop_back(); // remove the last space
  return args_joined;
}

void
append_start_failure_diagnostic(std::string_view stage,
                                std::string_view program_name,
                                std::vector<std::string> const &program_args,
                                std::string_view cpu_affinity,
                                std::string_view details)
{
  std::ofstream out("/tmp/ciserver_start_failure.log", std::ios::app);
  if (!out.is_open()) {
    return;
  }

  out << "ts=" << x23::fastclock::now().time_since_epoch().count()
      << " pid=" << getpid() << " stage=" << stage
      << " program=" << program_name << " args=[" << join_args(program_args)
      << "] cpu_affinity=" << cpu_affinity << " details=" << details << '\n';
}
} // namespace

class Subprocess {
public:
  Subprocess(std::string program_name, std::vector<std::string> program_args,
             std::string cpu_affinity)
    : program_name_(std::move(program_name)),
      program_args_(std::move(program_args)),
      cpu_affinity_(std::move(cpu_affinity))
  {}

  std::optional<pid_t>
  launch_process(std::string const &program_name,
                 std::vector<std::string> const &program_args,
                 std::string const &cpu_affinity)
  {
    std::string full_program_name =
        program_name.starts_with("/")
            ? program_name
            : std::string(CMAKE_ROOT_BINARY_DIR) + "/" + program_name;
    info("starting process: {} {}", full_program_name, join_args(program_args));

    std::optional<x23::CpuSet> cpu_set;
    if (!cpu_affinity.empty()) {
#if HAVE_SCHED_SETAFFINITY
      cpu_set = x23::parse_cpuset(cpu_affinity);
      if (!cpu_set->valid()) {
        error("invalid cpu affinity string: {}", cpu_affinity);
        append_start_failure_diagnostic("parse_cpuset", program_name,
                                        program_args, cpu_affinity,
                                        "invalid cpu affinity string");
        return std::nullopt;
      }
      info("applying cpu affinity: {}", cpu_affinity);
#else  // !HAVE_SCHED_SETAFFINITY
      panic("cpu affinity is unsupported on this platform: {}", cpu_affinity);
#endif // !HAVE_SCHED_SETAFFINITY
    }

    // Create temporary files
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::string cur_ts =
        std::to_string(x23::fastclock::now().time_since_epoch().count());
    std::filesystem::path stdout_path =
        temp_dir /
        ("hogd_stdout_" + cur_ts + "_" + std::to_string(getpid()) + "_XXXXXX");
    std::filesystem::path stderr_path =
        temp_dir /
        ("hogd_stderr_" + cur_ts + "_" + std::to_string(getpid()) + "_XXXXXX");

    // Convert to C-style strings for mkstemp
    std::string stdout_template = stdout_path.string();
    std::string stderr_template = stderr_path.string();

    // NOLINTBEGIN(misc-include-cleaner)
    int stdout_fd = mkstemp(stdout_template.data());
    int stderr_fd = mkstemp(stderr_template.data());
    // NOLINTEND(misc-include-cleaner)

    if (stdout_fd == -1 || stderr_fd == -1) {
      error("failed to create temporary files: {}", Errno{});
      append_start_failure_diagnostic("mkstemp", program_name, program_args,
                                      cpu_affinity, std::format("{}", Errno{}));
      if (stdout_fd != -1)
        close(stdout_fd);
      if (stderr_fd != -1)
        close(stderr_fd);
      return std::nullopt;
    }

    // Save the parent's original stdout/stderr before fork
    int parent_stdout = dup(STDOUT_FILENO);
    if (parent_stdout == -1) {
      error("failed to save parent stdout: {}", Errno{});
      append_start_failure_diagnostic("dup_stdout", program_name, program_args,
                                      cpu_affinity, std::format("{}", Errno{}));
      close(stdout_fd);
      close(stderr_fd);
      return std::nullopt;
    }

    int parent_stderr = dup(STDERR_FILENO);
    if (parent_stderr == -1) {
      error("failed to save parent stderr: {}", Errno{});
      append_start_failure_diagnostic("dup_stderr", program_name, program_args,
                                      cpu_affinity, std::format("{}", Errno{}));
      close(stdout_fd);
      close(stderr_fd);
      return std::nullopt;
    }

    // Print temporary file paths BEFORE fork to ensure parent logging works
    info("Child process output will be logged to:");
    info("  stdout: {}", stdout_template);
    info("  stderr: {}", stderr_template);

    auto pid = fork();
    if (pid < 0) {
      error("fork failed: {}", Errno{});
      append_start_failure_diagnostic("fork", program_name, program_args,
                                      cpu_affinity, std::format("{}", Errno{}));
      close(parent_stderr);
      close(parent_stdout);
      close(stdout_fd);
      close(stderr_fd);
      return std::nullopt;
    }

    if (pid == 0) {
      // Child process

      // Redirect stdout and stderr to temp files
      if (dup2(stdout_fd, STDOUT_FILENO) == -1 ||
          dup2(stderr_fd, STDERR_FILENO) == -1) {
        append_start_failure_diagnostic("dup2", full_program_name, program_args,
                                        cpu_affinity,
                                        std::format("{}", Errno{}));
        // NOLINTBEGIN(modernize-use-std-print)
        info("dup2 failed: %s\n", strerror(errno));
        // NOLINTEND(modernize-use-std-print)
        _exit(EXIT_FAILURE);
      }

      // Close the original file descriptors
      close(stdout_fd);
      close(stderr_fd);

      if (cpu_set.has_value()) {
        Errno err = cpu_set->set_affinity();
        if (err) {
          append_start_failure_diagnostic("sched_setaffinity",
                                          full_program_name, program_args,
                                          cpu_affinity, std::format("{}", err));
          info("sched_setaffinity failed: {}\n", err);
          _exit(EXIT_FAILURE);
        }
      }

      std::vector<char *> args;
      args.push_back(const_cast<char *>(program_name.data()));

      for (const auto &arg : program_args) {
        args.push_back(const_cast<char *>(arg.c_str()));
      }
      args.push_back(nullptr);

      // NOLINTBEGIN(modernize-use-std-print)
      info("execvp: {} {}\n", full_program_name, join_args(program_args));
      execvp(full_program_name.c_str(), args.data());
      append_start_failure_diagnostic("execvp", full_program_name, program_args,
                                      cpu_affinity, std::format("{}", Errno{}));
      info("execvp failed: %s\n", strerror(errno));
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

    // Start thread to wait for child process
    waitpid_thread_ =
        std::jthread([pid, this]() { wait_for_child_process(pid); });

    info("forked process: {}", pid);
    return pid;
  }

  std::optional<int> get_exit_code()
  {
    std::lock_guard lock(exit_code_mutex_);
    return exit_code_;
  }

  void wait_for_shutdown()
  {
    if (waitpid_thread_.joinable()) {
      waitpid_thread_.request_stop();
      waitpid_thread_.join();
    }
  }

private:
  void wait_for_child_process(pid_t child_pid)
  {
    info("waiting for child process: {}", child_pid);
    auto res = wait_for_child(child_pid);

    std::lock_guard lock(exit_code_mutex_);
    CHECK(!exit_code_.has_value());
    exit_code_ = res;
    if (exit_code_.has_value()) {
      info("child process: {} exited with code: {}", child_pid, *exit_code_);
      if (*exit_code_ != 0) {
        append_start_failure_diagnostic("child_exit", program_name_,
                                        program_args_, cpu_affinity_,
                                        std::format("exit_code={}",
                                                    *exit_code_));
      }
    }
    else {
      error("failed to wait for child process: {}", child_pid);
    }
  }

  std::mutex exit_code_mutex_;
  std::optional<int> exit_code_ = std::nullopt;

  std::jthread waitpid_thread_;
  std::string program_name_;
  std::vector<std::string> program_args_;
  std::string cpu_affinity_;
};

class CIControlRpc {
public:
  using rpc_service = CIServer1;

  CIControlRpc(std::string work_dir) : work_dir_(std::move(work_dir)) {}

  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~CIControlRpc()
  {
    // ensure all subprocesses are stopped
    ciserver_stop(hogd_git_hash_);
  }

  CIServerErrorCode ciserver_start(HogdGitHash const &hogd_git_hash,
                                   ServerConfig const &config)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIServerErrorCode::kCIServerErrorCodeVersionMismatch;
    }

    std::lock_guard lock(running_pid_mutex_);

    std::vector<std::string> program_args;
    for (auto const &arg : config.program_args) {
      program_args.emplace_back(arg.begin(), arg.end());
    }
    std::string cpu_affinity(config.cpu_affinity.begin(),
                             config.cpu_affinity.end());
    std::string program_name =
        std::string(config.program_name.begin(), config.program_name.end());
    std::unique_ptr<Subprocess> new_proc =
        std::make_unique<Subprocess>(program_name, program_args, cpu_affinity);
    info("launching process: {} {}", program_name, join_args(program_args));
    auto pid =
        new_proc->launch_process(program_name, program_args, cpu_affinity);
    if (!pid) {
      append_start_failure_diagnostic("launch_process", program_name,
                                      program_args, cpu_affinity,
                                      std::format("{}", Errno{}));
      error("failed to launch process: {}", Errno{});
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }
    subprocesses_.emplace(*pid, std::move(new_proc));
    most_recent_pid_ = pid;

    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  CIServerErrorCode ciserver_stop(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIServerErrorCode::kCIServerErrorCodeVersionMismatch;
    }

    std::lock_guard lock(running_pid_mutex_);

    for (auto &[pid, subprocess] : subprocesses_) {
      if (!subprocess->get_exit_code().has_value()) {
        info("stopping running process: {}", pid);
        kill(pid, SIGTERM); // NOLINT(misc-include-cleaner)
      }

      subprocess->wait_for_shutdown();
    }

    subprocesses_.clear();
    most_recent_pid_.reset();
    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  CIServerErrorCode ciserver_shutdown(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIServerErrorCode::kCIServerErrorCodeVersionMismatch;
    }

    info("stopping any running process");
    ciserver_stop(hogd_git_hash);

    info("shutting down control server");
    waiting_for_shutdown_ = true;
    waiting_for_shutdown_.notify_all();
    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  CIProcStatus ciserver_query_proc_status(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeVersionMismatch);
    }

    std::lock_guard<std::mutex> lock(running_pid_mutex_);
    if (most_recent_pid_.has_value()) {
      return check_pid_status(*most_recent_pid_, lock);
    }
    error("queried proc status before starting a process");
    return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeInternal);
  }

  CIWorkDirStatus ciserver_query_work_dir(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIWorkDirStatus(
          CIServerErrorCode::kCIServerErrorCodeVersionMismatch);
    }
    CIWorkDirStatus status(CIServerErrorCode::kCIServerErrorCodeOK);
    status.work_dir() = {work_dir_.begin(), work_dir_.end()};
    return status;
  }

  CIServerErrorCode ciserver_clear_work_dir(HogdGitHash const &hogd_git_hash)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIServerErrorCode::kCIServerErrorCodeVersionMismatch;
    }
    info("clearing work directory: {}", work_dir_);
    // clear the work directory
    try {
      std::filesystem::path dir_path(work_dir_);
      std::filesystem::remove_all(dir_path);
    } catch (const std::exception &e) {
      error("Exception clearing work directory {}: {}", work_dir_, e.what());
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }

    return CIServerErrorCode::kCIServerErrorCodeOK;
  }

  void wait_for_shutdown() { waiting_for_shutdown_.wait(false); }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

private:
  static std::string_view hash_sv(HogdGitHash const &h)
  {
    return {reinterpret_cast<const char *>(h.data()), h.size()};
  }

  static bool check_hogd_git_hash(HogdGitHash const &hogd_git_hash)
  {
    static HogdGitHash expected_hash = []() {
      auto str = std::string(version::kHogdGitHash);
      return x23::Buf{str.begin(), str.end()};
    }();
    return hogd_git_hash == expected_hash;
  }

  CIProcStatus check_pid_status(pid_t pid,
                                std::lock_guard<std::mutex> & /*unused*/)
  {
    auto it = subprocesses_.find(pid);
    if (it == subprocesses_.end()) {
      // queried info about a proc that doesn't exist
      return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeInternal);
    }
    if (it->second->get_exit_code().has_value()) {
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      CIProcStatus status(CIServerErrorCode::kCIServerErrorCodeOK);
      status.exit_code() = *it->second->get_exit_code();
      // NOLINTEND(bugprone-unchecked-optional-access)
      return status;
    }
    return CIProcStatus(CIServerErrorCode::kCIServerErrorCodeAlreadyRunning);
  }

  const HogdGitHash hogd_git_hash_ = []() {
    auto str = std::string(version::kHogdGitHash);
    return HogdGitHash{str.begin(), str.end()};
  }();

  std::atomic<bool> waiting_for_shutdown_ = false;

  std::mutex running_pid_mutex_;
  // NOLINTBEGIN(misc-include-cleaner)
  std::unordered_map<pid_t, std::unique_ptr<Subprocess>> subprocesses_;
  std::optional<pid_t> most_recent_pid_ = std::nullopt;
  // NOLINTEND(misc-include-cleaner)

  std::string work_dir_;
};

static_assert(is_CIServer1<CIControlRpc *, std::type_identity_t>);

class CIControlServer : public RpcServer<CIControlRpc> {
public:
  CIControlServer(std::string const &work_dir)
    : RpcServer<CIControlRpc>(work_dir)
  {}
};

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -p <control_port> -w <work_dir>", argv[0]);
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
  std::string work_dir;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "p:w:")) != -1) {
    switch (opt) {
    case 'p':
      control_port = std::stoi(optarg);
      break;
    case 'w':
      work_dir = optarg;
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!control_port) {
    control_port = hogd::kControlServerPort;
  }

  if (work_dir.empty()) {
    usage(argv);
  }

  std::filesystem::path p(work_dir);
  work_dir = std::filesystem::absolute(p).string();

  hogd::info("using work directory: {}", work_dir);

  hogd::CIControlServer control_server(work_dir);

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  control_server.start_server(*control_port);
  control_server->impl()->wait_for_shutdown();

  // This is a temporary bandaid until issue #597 is resolved.
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  return 0;
}
