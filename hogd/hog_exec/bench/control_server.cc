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
#include "hogd/build_config.h"
#include "hogd/hog_exec/bench/control_client.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "x23/config.h"
#include "x23/cpumap.h"
#include "x23/fastclock.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// NOLINTBEGIN(misc-include-cleaner)
// Observes the child's exit without reaping it. Leaving the zombie in place
// keeps the kernel from handing the pid to anyone else, and the pid doubles as
// the process group id every launched process is given, so a stop that arrives
// after the child has already died still has a group id it can safely signal.
// [reap_child] collects the zombie once the group has been dealt with.
std::optional<int>
wait_for_child(pid_t child_pid)
{
  siginfo_t child_info{};
  if (waitid(P_PID, static_cast<id_t>(child_pid), &child_info,
             WEXITED | WNOWAIT) == -1) {
    error("waitid failed for child {}: {}", child_pid, Errno{});
    return std::nullopt;
  }

  int exit_code = 0;

  if (child_info.si_code == CLD_EXITED) {
    exit_code = child_info.si_status;
    if (exit_code == 0) {
      info("Child process {} exited successfully with code {}", child_pid,
           exit_code);
    }
    else {
      error("Child process {} exited with error code {}", child_pid, exit_code);
    }
  }
  else if (child_info.si_code == CLD_KILLED ||
           child_info.si_code == CLD_DUMPED) {
    int signal_num = child_info.si_status;
    error("Child process {} terminated by signal {} ({})", child_pid,
          signal_num, strsignal(signal_num));
    // chatgpt says this matches the behavior of most shells
    exit_code = 128 + signal_num;
  }
  else {
    error("Child process {} terminated with unknown status {}", child_pid,
          child_info.si_code);
    exit_code = -1;
  }

  return exit_code;
}

// Collects the zombie left behind by [wait_for_child]. After this the pid, and
// the process group named after it, may be reused by anyone, so nothing may
// signal that group again.
void
reap_child(pid_t child_pid)
{
  if (waitpid(child_pid, nullptr, 0) == -1) {
    error("waitpid failed to reap child {}: {}", child_pid, Errno{});
  }
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
} // namespace

class Subprocess {
public:
  struct LaunchMetadata {
    pid_t pid;
    std::string stdout_path;
    std::string stderr_path;
  };

  std::optional<pid_t>
  launch_process(std::string const &program_name,
                 std::vector<std::string> const &program_args,
                 std::string const &cpu_affinity)
  {
    auto metadata =
        launch_process_with_metadata(program_name, program_args, cpu_affinity);
    if (!metadata) {
      return std::nullopt;
    }
    return metadata->pid;
  }

  std::optional<LaunchMetadata>
  launch_process_with_metadata(std::string const &program_name,
                               std::vector<std::string> const &program_args,
                               std::string const &cpu_affinity)
  {
    std::string full_program_name =
        program_name.starts_with("/")
            ? program_name
            : std::string(kBuildRoot) + "/" + program_name;
    info("starting process: {} {}", full_program_name, join_args(program_args));

    std::optional<x23::CpuSet> cpu_set;
    if (!cpu_affinity.empty()) {
#if HAVE_SCHED_SETAFFINITY
      cpu_set = x23::parse_cpuset(cpu_affinity);
      if (!cpu_set->valid()) {
        error("invalid cpu affinity string: {}", cpu_affinity);
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
      close(stdout_fd);
      close(stderr_fd);
      return std::nullopt;
    }

    int parent_stderr = dup(STDERR_FILENO);
    if (parent_stderr == -1) {
      error("failed to save parent stderr: {}", Errno{});
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
      close(parent_stderr);
      close(parent_stdout);
      close(stdout_fd);
      close(stderr_fd);
      return std::nullopt;
    }

    if (pid == 0) {
      // Child process

      // Put the child in a process group of its own, named after its pid, so
      // that [stop_process_group] can reach everything the launched program
      // spawns and not just the program itself. This is what a stop has to
      // signal: the benchmark scripts launch a shell that execs a server, and
      // some of those servers -- chuckd, which forks its own scpd -- leave
      // children behind that go on holding fixed ports after the server they
      // belong to is gone, wedging the next run's bind.
      //
      // Done in the child rather than only in the parent because the group has
      // to exist before execvp; the parent repeats it to close the race. A
      // failure here is fatal on purpose: a child left in the server's own
      // group would make a later group signal target the control server.
      if (setpgid(0, 0) == -1) {
        // NOLINTBEGIN(modernize-use-std-print)
        info("setpgid failed: {}", strerror(errno));
        // NOLINTEND(modernize-use-std-print)
        _exit(EXIT_FAILURE);
      }

      // Redirect stdout and stderr to temp files
      if (dup2(stdout_fd, STDOUT_FILENO) == -1 ||
          dup2(stderr_fd, STDERR_FILENO) == -1) {
        // NOLINTBEGIN(modernize-use-std-print)
        info("dup2 failed: {}", strerror(errno));
        // NOLINTEND(modernize-use-std-print)
        _exit(EXIT_FAILURE);
      }

      // Close the original file descriptors
      close(stdout_fd);
      close(stderr_fd);

      if (cpu_set.has_value()) {
        Errno err = cpu_set->set_affinity();
        if (err) {
          info("sched_setaffinity failed: {}", err);
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
      info("execvp: {} {}", full_program_name, join_args(program_args));
      execvp(full_program_name.c_str(), args.data());
      info("execvp failed: {}", strerror(errno));
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

    // The other half of the setpgid race: whichever of parent and child runs
    // first wins, and the loser sees EACCES (the child has already execvp'd)
    // or ESRCH (it is already gone). Neither is an error here -- the child
    // establishes the group itself and exits if it cannot -- so only an
    // unexpected errno is worth saying anything about.
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH) {
      error("failed to set process group for child {}: {}", pid, Errno{});
    }

    // Start thread to wait for child process
    child_pid_ = pid;
    waitpid_thread_ =
        std::jthread([pid, this]() { wait_for_child_process(pid); });

    info("forked process: {}", pid);
    return LaunchMetadata{
        .pid = pid,
        .stdout_path = stdout_template,
        .stderr_path = stderr_template,
    };
  }

  // Reaps the zombie [wait_for_child] deliberately leaves behind, for the
  // paths that drop a Subprocess without going through [ciserver_stop].
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~Subprocess() { wait_for_shutdown(); }

  std::optional<int> get_exit_code()
  {
    std::lock_guard lock(exit_code_mutex_);
    return exit_code_;
  }

  // Signals everything the launched program left running, itself included.
  // Sent to the whole group rather than the one pid the server forked, because
  // that pid is usually a shell that execs the real server, and the processes
  // that outlive a benchmark are the ones the server itself spawned. Safe to
  // call after the child has already exited -- [wait_for_child] leaves the
  // zombie in place until [wait_for_shutdown], so the group id cannot have
  // been reused by then, and an empty group just yields ESRCH.
  void stop_process_group()
  {
    if (child_pid_ < 0 || reaped_) {
      return;
    }

    // NOLINTBEGIN(misc-include-cleaner)
    if (getpgid(child_pid_) != child_pid_) {
      // setpgid did not take, so there is no group of our own to signal and a
      // group signal would land on the control server. Reach the child alone.
      error("child {} is not its own process group leader; signalling it alone",
            child_pid_);
      kill(child_pid_, SIGTERM);
      return;
    }

    if (kill(-child_pid_, SIGTERM) == -1 && errno != ESRCH) {
      error("failed to signal process group {}: {}", child_pid_, Errno{});
    }
    // NOLINTEND(misc-include-cleaner)
  }

  void wait_for_shutdown()
  {
    if (waitpid_thread_.joinable()) {
      waitpid_thread_.request_stop();
      waitpid_thread_.join();
    }
    if (child_pid_ >= 0 && !reaped_) {
      reap_child(child_pid_);
      reaped_ = true;
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
    }
    else {
      error("failed to wait for child process: {}", child_pid);
    }
  }

  std::mutex exit_code_mutex_;
  std::optional<int> exit_code_ = std::nullopt;

  pid_t child_pid_ = -1;
  bool reaped_ = false;

  std::jthread waitpid_thread_;
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

  CIStartStatus ciserver_start(HogdGitHash const &hogd_git_hash,
                               ServerConfig const &config)
  {
    if (!check_hogd_git_hash(hogd_git_hash)) {
      error("version mismatch: received {}, expected {}",
            hash_sv(hogd_git_hash), version::kHogdGitHash);
      return CIStartStatus(
          CIServerErrorCode::kCIServerErrorCodeVersionMismatch);
    }

    std::lock_guard lock(running_pid_mutex_);

    std::unique_ptr<Subprocess> new_proc = std::make_unique<Subprocess>();
    std::vector<std::string> program_args;
    for (auto const &arg : config.program_args) {
      program_args.emplace_back(arg.begin(), arg.end());
    }
    std::string cpu_affinity(config.cpu_affinity.begin(),
                             config.cpu_affinity.end());
    std::string program_name =
        std::string(config.program_name.begin(), config.program_name.end());
    info("launching process: {} {}", program_name, join_args(program_args));
    auto launch_metadata =
        new_proc->launch_process_with_metadata(program_name, program_args,
                                               cpu_affinity);
    if (!launch_metadata) {
      error("failed to launch process: {}", Errno{});
      return CIStartStatus(CIServerErrorCode::kCIServerErrorCodeInternal);
    }
    subprocesses_.emplace(launch_metadata->pid, std::move(new_proc));
    most_recent_pid_ = launch_metadata->pid;

    CIStartStatus status(CIServerErrorCode::kCIServerErrorCodeOK);
    status.paths().stdout_path = {launch_metadata->stdout_path.begin(),
                                  launch_metadata->stdout_path.end()};
    status.paths().stderr_path = {launch_metadata->stderr_path.begin(),
                                  launch_metadata->stderr_path.end()};
    return status;
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
      // Signalled whether or not the process this server forked is still
      // running: a server that has already died on its own -- crashed, or
      // exited after its work was done -- is exactly the case that leaves its
      // own children behind, and those are what the next run collides with.
      info("stopping process group: {}", pid);
      subprocess->stop_process_group();

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
