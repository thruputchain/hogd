#pragma once

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string.h> // NOLINT(modernize-deprecated-headers)
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/status.h" // for Errno
#include "hogd/build_config.h"
#include "x23/config.h"
#include "x23/cpumap.h"
#include "x23/fastclock.h"

namespace hogd {

// How long everything a launched process started gets to exit on SIGTERM
// before [Subprocess::reap] escalates to SIGKILL, and how long the SIGKILL
// then gets to take effect.
constexpr std::chrono::milliseconds kSubprocessTermGrace{10000};
constexpr std::chrono::milliseconds kSubprocessKillGrace{5000};

namespace detail {

// NOLINTBEGIN(misc-include-cleaner)
inline std::optional<int>
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

inline std::string
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

// The run state and process group of [pid], read out of /proc, if it is still
// there to read.
struct ProcState {
  char state;
  pid_t pgrp;
};

inline std::optional<ProcState>
read_proc_state(pid_t pid)
{
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(stat, line)) {
    return std::nullopt;
  }
  // The second field is the command name in parentheses, and it may contain
  // spaces and parentheses of its own, so the fields after it are found from
  // the last ')' rather than by counting from the front.
  auto comm_end = line.rfind(')');
  if (comm_end == std::string::npos) {
    return std::nullopt;
  }
  std::istringstream rest(line.substr(comm_end + 1));
  ProcState result{};
  pid_t ppid = 0;
  if (!(rest >> result.state >> ppid >> result.pgrp)) {
    return std::nullopt;
  }
  return result;
}

// True while any process in group [pgid] is still running.
//
// A process that has exited but has not yet been reaped stays a member of its
// group, and [kill] reports it as signallable. It is not something a stop has
// to wait for: it holds no ports, and no signal will move it along -- it goes
// when whoever inherited it gets around to reaping it, which is a wait with
// nothing at the end of it. So group membership is read from /proc, where a
// process that has exited can be told apart from one that has not. Where there
// is no /proc to read, [kill] gets the last word and zombies are waited out.
inline bool
process_group_alive(pid_t pgid)
{
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (kill(-pgid, 0) == -1) {
    // EPERM means the group holds something this process may not signal; that
    // is still a survivor, and one this process cannot clear.
    return errno == EPERM;
  }

  DIR *proc = opendir("/proc"); // NOLINT(misc-include-cleaner)
  if (proc == nullptr) {
    return true;
  }

  bool alive = false;
  // NOLINTNEXTLINE(misc-include-cleaner)
  while (dirent *entry = readdir(proc)) {
    pid_t pid = 0;
    char const *name = entry->d_name;
    auto parsed = std::from_chars(name, name + strlen(name), pid);
    if (parsed.ec != std::errc{} || *parsed.ptr != '\0' || pid <= 0) {
      continue; // not a process directory
    }
    auto state = read_proc_state(pid);
    if (state && state->pgrp == pgid && state->state != 'Z') {
      alive = true;
      break;
    }
  }
  closedir(proc); // NOLINT(misc-include-cleaner)
  return alive;
}

// Waits up to [timeout] for process group [pgid] to empty out, polling because
// there is no way to wait on a group this process is not the parent of.
// Returns true if the group is gone.
inline bool
wait_for_process_group(pid_t pgid, std::chrono::milliseconds timeout)
{
  constexpr std::chrono::milliseconds kPollInterval{20};
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (process_group_alive(pgid)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return true;
}

} // namespace detail

// A single process launched by the control server, together with everything
// that process starts.
//
// The launched process is made the leader of its own process group, so that
// stopping it stops its descendants too. Signalling the process alone is not
// enough: the benchmark scripts hand this server a `bash -lc` wrapper or a
// program that forks helpers of its own (chuckd forks scpd), and a helper that
// outlives the suite keeps holding the fixed ports the next suite binds.
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
    info("starting process: {} {}", full_program_name,
         detail::join_args(program_args));

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

      // Lead a new process group, so that stopping this process stops
      // everything it starts. Done here as well as in the parent because
      // either side alone loses a race: the parent's call can land after an
      // exec that has already forked, and this one is undone by nothing but
      // its own failure.
      // NOLINTNEXTLINE(misc-include-cleaner)
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
      info("execvp: {} {}", full_program_name, detail::join_args(program_args));
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

    pid_ = pid;
    // EACCES means the child got there first and has already exec'd, which is
    // the outcome this call wants; anything else leaves the child in this
    // server's group, where a group signal must not be aimed.
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setpgid(pid, pid) == -1 && errno != EACCES) {
      error("setpgid failed for child {}: {}", pid, Errno{});
      own_process_group_ = false;
    }
    else {
      own_process_group_ = true;
    }

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

  // Asks the launched process and everything it started to exit. Sent to the
  // whole process group even once the launched process itself is gone: a
  // helper it forked is orphaned by its exit, not ended by it.
  void terminate()
  {
    if (pid_ == -1) {
      return;
    }
    if (own_process_group_) {
      info("stopping process group: {}", pid_);
    }
    else if (!get_exit_code().has_value()) {
      info("stopping running process: {}", pid_);
    }
    signal_all(SIGTERM);
  }

  // Waits for the launched process to exit and collects its status.
  //
  // [terminate] is a request, and this is where a process that declines it is
  // dealt with: waiting on one unconditionally would hang the stop that called
  // it -- holding the lock the control server needs to start anything else --
  // and never reach [reap], which is where the SIGKILL lives. So the wait is
  // bounded, and a process still running at the end of it is killed outright.
  void
  wait_for_shutdown(std::chrono::milliseconds term_grace = kSubprocessTermGrace)
  {
    if (!waitpid_thread_.joinable()) {
      return;
    }
    if (pid_ != -1 && !await_exit(term_grace)) {
      error("process {} outlived SIGTERM after {}ms; sending SIGKILL", pid_,
            term_grace.count());
      signal_all(SIGKILL);
    }
    // SIGKILL cannot be declined, so the join is bounded by whatever the
    // kernel needs to tear the process down.
    waitpid_thread_.request_stop();
    waitpid_thread_.join();
  }

  // Waits out whatever the launched process left behind, escalating to
  // SIGKILL. Callers of stop bind the same fixed ports moments later, so a
  // stop that returned while a leftover still held one would be a stop in name
  // only. Returns false if anything is still there after the SIGKILL.
  //
  // Must be called after [wait_for_shutdown]: the launched process itself is
  // in the group, and until it has been reaped it is a member like any other.
  bool reap(std::chrono::milliseconds term_grace = kSubprocessTermGrace,
            std::chrono::milliseconds kill_grace = kSubprocessKillGrace)
  {
    if (pid_ == -1 || !own_process_group_) {
      return true;
    }
    if (detail::wait_for_process_group(pid_, term_grace)) {
      return true;
    }

    error("process group {} outlived SIGTERM after {}ms; sending SIGKILL", pid_,
          term_grace.count());
    kill(-pid_, SIGKILL); // NOLINT(misc-include-cleaner)
    if (detail::wait_for_process_group(pid_, kill_grace)) {
      return true;
    }

    error("process group {} survived SIGKILL", pid_);
    return false;
  }

private:
  // Signals the launched process and everything it started, or just the
  // process itself if it never got a group of its own -- signalling the group
  // it is in instead would be signalling this server.
  void signal_all(int sig)
  {
    if (pid_ == -1) {
      return;
    }
    if (own_process_group_) {
      kill(-pid_, sig); // NOLINT(misc-include-cleaner)
    }
    else if (!get_exit_code().has_value()) {
      kill(pid_, sig); // NOLINT(misc-include-cleaner)
    }
  }

  // Waits up to [timeout] for the launched process to exit. Polled rather than
  // waited on: [waitpid] is the [waitpid_thread_]'s to call, and this is what
  // is left to a second caller that must not block on it forever.
  bool await_exit(std::chrono::milliseconds timeout)
  {
    constexpr std::chrono::milliseconds kPollInterval{20};
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!get_exit_code().has_value()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    return true;
  }

  void wait_for_child_process(pid_t child_pid)
  {
    info("waiting for child process: {}", child_pid);
    auto res = detail::wait_for_child(child_pid);

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

  // NOLINTBEGIN(misc-include-cleaner)
  pid_t pid_ = -1;
  // NOLINTEND(misc-include-cleaner)
  // Whether [pid_] leads its own process group, and so whether [pid_] may be
  // used as a group id. False only if setpgid failed, which leaves the child
  // in this server's own group -- signalling that group would take down the
  // control server with it.
  bool own_process_group_ = false;

  std::jthread waitpid_thread_;
};

} // namespace hogd
