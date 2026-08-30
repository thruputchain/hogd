#pragma once

#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
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
    : pid_(other.pid_), logs_(std::move(other.logs_))
  {
    other.pid_ = -1;
  }

  DaemonProcess &operator=(DaemonProcess &&other) noexcept
  {
    if (this != &other) {
      reset();
      pid_ = other.pid_;
      logs_ = std::move(other.logs_);
      other.pid_ = -1;
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
    if (!logs_) {
      throw std::runtime_error("process log reader is not running");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(logs_->mu);
    while (logs_->text.find(needle) == std::string::npos) {
      if (!logs_->error.empty()) {
        throw std::runtime_error(logs_->error);
      }
      if (logs_->eof) {
        throw std::runtime_error(
            std::format("process exited before matching '{}': {}", needle,
                        logs_->text));
      }
      if (logs_->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
        if (logs_->text.find(needle) != std::string::npos) {
          return;
        }
        throw std::runtime_error(
            std::format("timed out waiting for '{}': {}", needle, logs_->text));
      }
    }
  }

  std::uint16_t require_port(std::string_view key) const
  {
    if (!logs_) {
      throw std::runtime_error("process log reader is not running");
    }
    std::lock_guard lock(logs_->mu);
    const std::string marker = std::format("PORT {}=", key);
    const std::size_t pos = logs_->text.find(marker);
    if (pos == std::string::npos) {
      throw std::runtime_error(
          std::format("missing {} in process log: {}", marker, logs_->text));
    }
    const std::size_t start = pos + marker.size();
    const std::size_t end = logs_->text.find_first_not_of("0123456789", start);
    const std::string digits = logs_->text.substr(
        start, (end == std::string::npos ? logs_->text.size() : end) - start);
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

private:
  static constexpr std::size_t kMaxLogBytes = 256 * 1024;

  struct LogState {
    int fd = -1;
    std::mutex mu;
    std::condition_variable cv;
    std::string text;
    std::string error;
    bool eof = false;
    std::thread reader;

    ~LogState()
    {
      if (reader.joinable()) {
        reader.join();
      }
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }
  };

  static std::string errno_message(std::string_view operation, int err)
  {
    return std::format("{}: {}", operation,
                       std::generic_category().message(err));
  }

  static void run_reader(LogState *state)
  {
    char buffer[1024];
    for (;;) {
      const ssize_t count = read(state->fd, buffer, sizeof(buffer));
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        std::lock_guard lock(state->mu);
        state->error = errno_message("read", errno);
        state->eof = true;
        state->cv.notify_all();
        return;
      }
      std::lock_guard lock(state->mu);
      if (count == 0) {
        state->eof = true;
        state->cv.notify_all();
        return;
      }
      const auto room = kMaxLogBytes > state->text.size()
                            ? kMaxLogBytes - state->text.size()
                            : 0;
      if (room > 0) {
        state->text.append(buffer,
                           std::min(static_cast<std::size_t>(count), room));
      }
      state->cv.notify_all();
    }
  }

  DaemonProcess(pid_t pid, int log_fd)
    : pid_(pid), logs_(std::make_unique<LogState>())
  {
    logs_->fd = log_fd;
    logs_->reader = std::thread(&DaemonProcess::run_reader, logs_.get());
  }

  void reset()
  {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      int status = 0;
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
    }
    logs_.reset();
  }

  std::optional<int> wait_for_exit(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      const pid_t result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        if (logs_ && logs_->reader.joinable()) {
          logs_->reader.join();
        }
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

  pid_t pid_ = -1;
  std::unique_ptr<LogState> logs_;
};

} // namespace hogd
