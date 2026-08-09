#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
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

// Darwin's POSIX signal declarations are not fully modeled by include-cleaner.
// NOLINTBEGIN(misc-include-cleaner)

// Set by the SIGTERM/SIGINT handler. A handler may touch nothing else: this is
// the whole of what runs in signal context, and [wait_for_shutdown] polls it.
volatile std::sig_atomic_t g_termination_requested =
    0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

bool
termination_requested()
{
  return g_termination_requested != 0;
}

void
on_termination_signal(int /*signal_number*/)
{
  g_termination_requested = 1;
}
// NOLINTEND(misc-include-cleaner)

// How long [stop] waits for a process to leave on SIGTERM before escalating,
// and how long it then waits for SIGKILL to take effect. A process that is
// going to leave does so at once, so these are only ever paid by one that is
// not, and they are what turns "the signal was delivered" into "the host is
// clear".
constexpr auto kStopTermGrace = std::chrono::seconds(5);
constexpr auto kStopKillGrace = std::chrono::seconds(5);
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

      // Lead a new process group, so [stop] can signal everything this launch
      // spawns rather than only the process we forked. The benchmark launches
      // that go through [bash -lc "... exec ..."] end up as the workload
      // itself, but a launch that spawns helpers of its own -- [chuckd.dev]
      // starts [scpd.dev] -- leaves them out of a signal addressed to the pid
      // alone, and a helper that outlives the run keeps its sockets.
      //
      // Both the child and the parent set the group: whichever runs first
      // wins, so there is no window in which the parent can signal a pid that
      // is not yet in its own group.
      if (setpgid(0, 0) == -1) {
        // NOLINTBEGIN(modernize-use-std-print)
        info("setpgid failed: {}", strerror(errno));
        // NOLINTEND(modernize-use-std-print)
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

    pid_ = pid;
    // EACCES means the child has already exec'd, which it can only do after
    // running its own [setpgid]: the group exists either way. Any other error
    // leaves the child in this server's group, where a group-directed signal
    // would hit the server itself, so we fall back to signalling the pid.
    if (setpgid(pid, pid) == 0 || errno == EACCES) {
      pgid_ = pid;
    }
    else {
      error("setpgid failed for child {}: {}; will signal the pid alone", pid,
            Errno{});
    }

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
    return LaunchMetadata{
        .pid = pid,
        .stdout_path = stdout_template,
        .stderr_path = stderr_template,
    };
  }

  std::optional<int> get_exit_code()
  {
    std::lock_guard lock(exit_code_mutex_);
    return exit_code_;
  }

  pid_t pid() const { return pid_; }

  // Signals the launch. Addresses the whole process group when the child leads
  // one, so helpers it spawned are signalled too.
  void signal_process(int sig)
  {
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (pgid_.has_value() && kill(-*pgid_, sig) == 0) {
      return;
    }
    // The group can be empty while the child is still an unreaped zombie, and
    // it does not exist at all if [setpgid] failed. Either way the pid is the
    // thing we know about.
    kill(pid_, sig); // NOLINT(misc-include-cleaner)
  }

  // True once nothing this launch put on the host is left. The forked leader
  // being reaped is not enough: a helper it spawned holds its sockets just the
  // same, and [chuckd.dev] exits on SIGTERM well before the [scpd.dev] it
  // started does.
  bool is_stopped()
  {
    {
      std::lock_guard lock(exit_code_mutex_);
      if (!reaped_) {
        return false;
      }
    }
    return group_is_empty();
  }

  // Waits until [is_stopped] holds or the deadline passes. Reports whether the
  // launch is gone.
  bool wait_until_stopped(std::chrono::steady_clock::time_point deadline)
  {
    {
      std::unique_lock lock(exit_code_mutex_);
      if (!exited_.wait_until(lock, deadline, [this] { return reaped_; })) {
        return false;
      }
    }
    // The group has no equivalent of [waitpid], so this is a poll. It only
    // runs after the leader is gone, which for a launch with no helpers means
    // one check that passes immediately.
    while (!group_is_empty()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
  }

  void wait_for_shutdown()
  {
    if (waitpid_thread_.joinable()) {
      waitpid_thread_.request_stop();
      waitpid_thread_.join();
    }
  }

private:
  // Whether anything is left in the launch's process group. A group we cannot
  // signal is treated as still populated: the one answer this must never
  // invent is "nothing is there".
  bool group_is_empty() const
  {
    if (!pgid_.has_value()) {
      // No group was established, so the pid is all this launch ever tracked
      // and the caller has already established that it was reaped.
      return true;
    }
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (kill(-*pgid_, 0) == 0) {
      return false;
    }
    return errno == ESRCH;
  }

  void wait_for_child_process(pid_t child_pid)
  {
    info("waiting for child process: {}", child_pid);
    auto res = wait_for_child(child_pid);

    std::lock_guard lock(exit_code_mutex_);
    CHECK(!exit_code_.has_value());
    exit_code_ = res;
    // [waitpid] has returned, so the child is gone whether or not it told us
    // how. A failed wait must not read as "still running" to [stop]: that
    // would escalate to SIGKILL and then report a survivor for a process that
    // no longer exists.
    reaped_ = true;
    if (exit_code_.has_value()) {
      info("child process: {} exited with code: {}", child_pid, *exit_code_);
    }
    else {
      error("failed to wait for child process: {}", child_pid);
    }
    exited_.notify_all();
  }

  pid_t pid_ = -1;
  std::optional<pid_t> pgid_ = std::nullopt;

  std::mutex exit_code_mutex_;
  std::condition_variable exited_;
  bool reaped_ = false;
  std::optional<int> exit_code_ = std::nullopt;

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

    // Sending SIGTERM is not the same as the launch being gone, and the caller
    // of [stop] launches the next benchmark suite onto the same ports the
    // moment this returns. A process that ignores SIGTERM, or that is simply
    // slower to leave than the caller is to move on, keeps its listening
    // sockets: the next suite then dies with "Address already in use", and the
    // readiness wait that eventually reports it names the first port nothing
    // happened to be holding rather than the host that was busy. Wait for each
    // launch to actually leave, escalate to SIGKILL if it does not, and tell
    // the caller when even that was not enough.
    //
    // The two graces are deadlines for the whole stop rather than budgets per
    // launch, so the wall-clock cost does not scale with the number of
    // processes. That bound is load-bearing: a stop reached through SIGTERM is
    // racing the caller's own escalation, and a server killed part way through
    // orphans exactly the processes it was clearing.
    std::vector<Subprocess *> pending;

    // Anything a previous stop could not kill is still this host's problem, so
    // re-signal it and re-check it here rather than reporting a clear host on
    // the second call.
    std::erase_if(unstoppable_, [](Subprocess *subprocess) {
      return subprocess->is_stopped();
    });
    for (auto *subprocess : unstoppable_) {
      subprocess->signal_process(SIGKILL); // NOLINT(misc-include-cleaner)
      pending.push_back(subprocess);
    }

    for (auto &[pid, subprocess] : subprocesses_) {
      if (subprocess->is_stopped()) {
        continue;
      }
      info("stopping running process: {}", pid);
      subprocess->signal_process(SIGTERM); // NOLINT(misc-include-cleaner)
      pending.push_back(subprocess.get());
    }

    auto term_deadline = std::chrono::steady_clock::now() + kStopTermGrace;
    std::erase_if(pending, [term_deadline](Subprocess *subprocess) {
      return subprocess->wait_until_stopped(term_deadline);
    });

    if (!pending.empty()) {
      for (auto *subprocess : pending) {
        error("process {} is still running {}s after SIGTERM; sending SIGKILL",
              subprocess->pid(), kStopTermGrace.count());
        subprocess->signal_process(SIGKILL); // NOLINT(misc-include-cleaner)
      }
      auto kill_deadline = std::chrono::steady_clock::now() + kStopKillGrace;
      std::erase_if(pending, [kill_deadline](Subprocess *subprocess) {
        return subprocess->wait_until_stopped(kill_deadline);
      });
    }

    std::vector<pid_t> survivors;
    for (auto *subprocess : pending) {
      error("process {} is still running {}s after SIGKILL", subprocess->pid(),
            kStopKillGrace.count());
      survivors.push_back(subprocess->pid());
    }

    for (auto it = subprocesses_.begin(); it != subprocesses_.end();) {
      if (std::ranges::find(survivors, it->first) != survivors.end()) {
        // Destroying this would join a [waitpid] thread that is still blocked
        // on a process that has not exited, hanging the server on the very
        // request meant to clean it up. Hand it to [unstoppable_], which never
        // destroys what it holds; the next stop re-checks it, and the waitpid
        // thread reaps the process if it ever does leave.
        unstoppable_.push_back(it->second.release());
        it = subprocesses_.erase(it);
        continue;
      }
      it->second->wait_for_shutdown();
      it = subprocesses_.erase(it);
    }

    most_recent_pid_.reset();

    // A launch this stop finally cleared is no longer a reason to report the
    // host dirty.
    std::erase_if(unstoppable_, [](Subprocess *subprocess) {
      return subprocess->is_stopped();
    });

    if (!unstoppable_.empty()) {
      error("could not stop {} process(es) on this host", unstoppable_.size());
      return CIServerErrorCode::kCIServerErrorCodeInternal;
    }
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
    auto stop_status = ciserver_stop(hogd_git_hash);

    // The server comes down either way -- a host it could not clear is not a
    // reason to leave the control server itself running -- but the caller is
    // told, because "shutdown succeeded" on a host that still has a benchmark
    // process holding its ports is the answer that sends the next suite into
    // a bind failure.
    info("shutting down control server");
    waiting_for_shutdown_ = true;
    return stop_status;
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

  // Stops everything this server launched. Bypasses the version check because
  // this is the server winding down its own children, not a client asking it
  // to.
  CIServerErrorCode stop_all_processes()
  {
    return ciserver_stop(hogd_git_hash_);
  }

  void wait_for_shutdown()
  {
    while (!waiting_for_shutdown_.load() && !termination_requested()) {
      // Polled rather than waited on, so the signal handler that ends this
      // loop has nothing to do but set a flag. Nothing here is latency
      // sensitive: the shutdown RPC and a SIGTERM both arrive once, at the end
      // of a run.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

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

  // Launches that survived SIGKILL. Deliberately never destroyed: their
  // [waitpid] threads are still blocked on processes that have not exited, and
  // joining one would hang the server. Only [ciserver_stop] touches this, and
  // it never grows beyond the number of launches that would not die.
  std::vector<Subprocess *> unstoppable_;

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

  // A control server that is asked to go away must take the benchmark
  // processes it launched with it. Without a handler, SIGTERM ends this
  // process outright: nothing stops the children, and they outlive the run
  // still holding their listening sockets. The next run's server has no record
  // of them -- it did not launch them -- so the suite that binds those ports
  // dies with "Address already in use" on a host nobody has any reason to
  // suspect. The CI setup reaps a leftover server with exactly this signal
  // (`killall -TERM control_server`) before starting its own, so this is the
  // path the orphans come from.
  // NOLINTBEGIN(misc-include-cleaner)
  std::signal(SIGTERM, hogd::on_termination_signal);
  std::signal(SIGINT, hogd::on_termination_signal);
  // NOLINTEND(misc-include-cleaner)

  hogd::CIControlServer control_server(work_dir);

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  control_server.start_server(*control_port);
  control_server->impl()->wait_for_shutdown();

  hogd::info("stopping launched processes before exiting");
  auto stop_status = control_server->impl()->stop_all_processes();

  // This is a temporary bandaid until issue #597 is resolved.
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  if (stop_status != hogd::CIServerErrorCode::kCIServerErrorCodeOK) {
    // Exiting successfully here would leave the host dirty and say nothing,
    // which is how a benchmark process outlives its run in the first place.
    hogd::error("could not stop every launched process before exiting");
    return EXIT_FAILURE;
  }
  return 0;
}
