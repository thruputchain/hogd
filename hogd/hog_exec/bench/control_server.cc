#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unistd.h> // NOLINT(misc-include-cleaner)
#include <unordered_map>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/status.h"
#include "hogd/hog_exec/bench/control_client.h"
#include "hogd/hog_exec/bench/subprocess.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "x23/vec.h"

namespace hogd {

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
    info("launching process: {} {}", program_name,
         detail::join_args(program_args));
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

    // Signal everything first, so that several subprocesses serve out their
    // grace periods together rather than one after another.
    for (auto &entry : subprocesses_) {
      entry.second->terminate();
    }

    for (auto &[pid, subprocess] : subprocesses_) {
      subprocess->wait_for_shutdown();
      if (!subprocess->reap()) {
        error("processes started by {} could not be stopped", pid);
      }
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

  // NOLINTNEXTLINE(misc-include-cleaner)
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
