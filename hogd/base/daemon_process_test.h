#pragma once

#include <poll.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace hogd {

// Forks a production hog-* binary, captures combined stdout/stderr, and
// SIGKILLs it. Used by component-restart tests that must exercise the real
// process image rather than an in-process server object.
class DaemonProcess {
public:
  static constexpr auto kDefaultStartupTimeout = std::chrono::seconds(30);
  static constexpr auto kShutdownTimeout = std::chrono::seconds(5);

  static DaemonProcess start(const char *binary,
                             const std::vector<std::string> &args)
  {
    // Build argv in the parent. After fork() in a multithreaded test
    // process (RpcServer worker threads), the child may only use
    // async-signal-safe calls until execv.
    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.emplace_back(binary);
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());
    std::vector<char *> argv;
    argv.reserve(argv_storage.size() + 1);
    for (std::string &arg : argv_storage) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    int log_pipe[2];
    if (pipe(log_pipe) != 0) {
      throw std::runtime_error(errno_message("pipe", errno));
    }

    const pid_t pid = fork();
    if (pid < 0) {
      const int saved = errno;
      close(log_pipe[0]);
      close(log_pipe[1]);
      throw std::runtime_error(errno_message("fork", saved));
    }
    if (pid == 0) {
      close(log_pipe[0]);
      if (dup2(log_pipe[1], STDOUT_FILENO) < 0 ||
          dup2(log_pipe[1], STDERR_FILENO) < 0) {
        _exit(127);
      }
      close(log_pipe[1]);
      execv(argv[0], argv.data());
      _exit(127);
    }

    close(log_pipe[1]);
    return DaemonProcess(pid, log_pipe[0]);
  }

  DaemonProcess(DaemonProcess &&other) noexcept
    : pid_(other.pid_), log_fd_(other.log_fd_), log_(std::move(other.log_))
  {
    other.pid_ = -1;
    other.log_fd_ = -1;
  }

  DaemonProcess &operator=(DaemonProcess &&other) noexcept
  {
    if (this != &other) {
      reset();
      pid_ = other.pid_;
      log_fd_ = other.log_fd_;
      log_ = std::move(other.log_);
      other.pid_ = -1;
      other.log_fd_ = -1;
    }
    return *this;
  }

  ~DaemonProcess() { reset(); }

  DaemonProcess(const DaemonProcess &) = delete;
  DaemonProcess &operator=(const DaemonProcess &) = delete;

  void wait_until_log_contains(
      std::string_view needle,
      std::chrono::milliseconds timeout = kDefaultStartupTimeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (log_.find(needle) == std::string::npos) {
      if (exited()) {
        throw std::runtime_error(
            std::format("process exited before matching '{}': {}", needle,
                        log_));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error(
            std::format("timed out waiting for '{}': {}", needle, log_));
      }
      read_available(deadline);
    }
  }

  std::uint16_t require_port(std::string_view key) const
  {
    const std::string marker = std::format("PORT {}=", key);
    const std::size_t pos = log_.find(marker);
    if (pos == std::string::npos) {
      throw std::runtime_error(
          std::format("missing {} in process log: {}", marker, log_));
    }
    const std::size_t start = pos + marker.size();
    const std::size_t end = log_.find_first_not_of("0123456789", start);
    const std::string digits =
        log_.substr(start,
                    (end == std::string::npos ? log_.size() : end) - start);
    const std::uint64_t port = std::stoull(digits);
    if (port == 0 || port > 65535) {
      throw std::runtime_error(std::format("invalid port in {}", marker));
    }
    return static_cast<std::uint16_t>(port);
  }

  int kill_unclean()
  {
    if (pid_ <= 0) {
      throw std::runtime_error("process already reaped");
    }
    if (kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
      throw std::runtime_error(errno_message("kill SIGKILL", errno));
    }
    std::optional<int> status = wait_for_exit(kShutdownTimeout);
    if (!status.has_value()) {
      throw std::runtime_error("process did not exit after SIGKILL");
    }
    if (!WIFSIGNALED(*status)) {
      throw std::runtime_error(
          std::format("process did not die from a signal (status={})",
                      *status));
    }
    return WTERMSIG(*status);
  }

  const std::string &log() const { return log_; }

private:
  static constexpr std::size_t kMaxLogBytes = 256 * 1024;

  static std::string errno_message(std::string_view operation, int err)
  {
    return std::format("{}: {}", operation,
                       std::generic_category().message(err));
  }

  DaemonProcess(pid_t pid, int log_fd) : pid_(pid), log_fd_(log_fd) {}

  void reset()
  {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      int status = 0;
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
    }
    if (log_fd_ >= 0) {
      close(log_fd_);
      log_fd_ = -1;
    }
  }

  bool exited()
  {
    if (pid_ <= 0) {
      return true;
    }
    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      pid_ = -1;
      drain();
      return true;
    }
    if (result < 0 && errno != EINTR) {
      throw std::runtime_error(errno_message("waitpid", errno));
    }
    return false;
  }

  std::optional<int> wait_for_exit(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      const pid_t result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        drain();
        return status;
      }
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error(errno_message("waitpid", errno));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void drain()
  {
    if (log_fd_ < 0) {
      return;
    }
    char buffer[1024];
    for (;;) {
      const ssize_t count = read(log_fd_, buffer, sizeof(buffer));
      if (count > 0) {
        append_log(buffer, static_cast<std::size_t>(count));
        continue;
      }
      break;
    }
  }

  void read_available(std::chrono::steady_clock::time_point deadline)
  {
    if (log_fd_ < 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    // NOLINTNEXTLINE(misc-include-cleaner)
    pollfd poll_fd{.fd = log_fd_, .events = POLLIN, .revents = 0};
    // NOLINTNEXTLINE(misc-include-cleaner)
    const int ready = poll(&poll_fd, 1, static_cast<int>(remaining.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        return;
      }
      throw std::runtime_error(errno_message("poll", errno));
    }
    if (ready == 0) {
      return;
    }
    char buffer[1024];
    const ssize_t count = read(log_fd_, buffer, sizeof(buffer));
    if (count > 0) {
      append_log(buffer, static_cast<std::size_t>(count));
      return;
    }
    if (count == 0) {
      close(log_fd_);
      log_fd_ = -1;
      return;
    }
    if (errno != EINTR) {
      throw std::runtime_error(errno_message("read", errno));
    }
  }

  void append_log(const char *buffer, std::size_t count)
  {
    log_.append(buffer, count);
    if (log_.size() > kMaxLogBytes) {
      throw std::runtime_error("process log exceeded limit");
    }
  }

  pid_t pid_ = -1;
  int log_fd_ = -1;
  std::string log_;
};

} // namespace hogd
