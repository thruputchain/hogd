#include <fcntl.h>
#include <sys/poll.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <print>
#include <string>
#include <string_view>

#ifndef HOG_DEVNODE_BINARY
#error "HOG_DEVNODE_BINARY must be defined by CMake"
#endif

namespace {

constexpr auto kDefaultStartupTimeout = std::chrono::seconds(30);
constexpr auto kShutdownTimeout = std::chrono::seconds(5);
constexpr std::size_t kMaxCapturedStderr = 256 * 1024;

std::string
json_string_field(std::string_view json, std::string_view field)
{
  std::string prefix = "\"" + std::string(field) + "\":\"";
  std::size_t start = json.find(prefix);
  if (start == std::string_view::npos) {
    return {};
  }
  start += prefix.size();
  std::size_t end = json.find('"', start);
  if (end == std::string_view::npos) {
    return {};
  }
  return std::string(json.substr(start, end - start));
}

// Honor HOG_DEVNODE_STARTUP_TIMEOUT_SECS just like hog_devnode_test.cc, so slow
// sanitizer schedules (e.g. TSAN, which sets it to 180) can give hog-devnode
// enough time to reach readiness. Falls back to the default when unset.
std::chrono::seconds
startup_timeout()
{
  const char *value = std::getenv("HOG_DEVNODE_STARTUP_TIMEOUT_SECS");
  if (value == nullptr || *value == '\0') {
    return kDefaultStartupTimeout;
  }

  errno = 0;
  char *end = nullptr;
  std::uint64_t seconds = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || seconds == 0 ||
      seconds > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    std::println(stderr, "invalid HOG_DEVNODE_STARTUP_TIMEOUT_SECS value");
    std::exit(EXIT_FAILURE);
  }
  return std::chrono::seconds(static_cast<int>(seconds));
}

// NOLINTBEGIN(misc-include-cleaner)
void
close_if_open(int &fd)
{
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

void
fail_with_errno(std::string_view op)
{
  std::println(stderr, "{:.{}}: {}", op.data(), static_cast<int>(op.size()),
               std::strerror(errno));
  std::exit(EXIT_FAILURE);
}

void
set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    fail_with_errno("fcntl");
  }
}

std::string
read_available(int fd)
{
  std::string output;
  char buffer[4096];
  for (;;) {
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      return output;
    }
    if (errno == EINTR) {
      continue;
    }
    fail_with_errno("read");
  }
}

bool
wait_for_exit(pid_t pid, std::chrono::milliseconds timeout, int &status)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      return true;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      fail_with_errno("waitpid");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    usleep(10'000);
  }
}

void
cleanup_child(pid_t pid)
{
  kill(pid, SIGTERM);
  int status = 0;
  if (!wait_for_exit(pid, kShutdownTimeout, status)) {
    kill(pid, SIGKILL);
    if (!wait_for_exit(pid, kShutdownTimeout, status)) {
      std::println(stderr, "hog-devnode did not exit after SIGKILL");
    }
  }
}
} // namespace

int
main() // NOLINT(bugprone-exception-escape)
{
  int stdout_pipe[2];
  int stderr_pipe[2];
  if (pipe(stdout_pipe) != 0) {
    fail_with_errno("pipe stdout");
  }
  if (pipe(stderr_pipe) != 0) {
    fail_with_errno("pipe stderr");
  }

  pid_t pid = fork();
  if (pid < 0) {
    fail_with_errno("fork");
  }

  if (pid == 0) {
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    setenv("HOGD_LOG_FORMAT", "json", /*overwrite=*/1);

    char *argv[] = {
        const_cast<char *>(HOG_DEVNODE_BINARY),
        const_cast<char *>("--listen-host"),
        const_cast<char *>("127.0.0.1"),
        const_cast<char *>("--scheduler-port"),
        const_cast<char *>("0"),
        const_cast<char *>("--readiness"),
        const_cast<char *>("stdout"),
        nullptr,
    };
    execv(HOG_DEVNODE_BINARY, argv);
    _exit(127);
  }

  close_if_open(stdout_pipe[1]);
  close_if_open(stderr_pipe[1]);
  set_nonblocking(stdout_pipe[0]);
  set_nonblocking(stderr_pipe[0]);

  std::string stdout_output;
  std::string stderr_output;
  const auto deadline = std::chrono::steady_clock::now() + startup_timeout();
  bool saw_readiness = false;
  bool child_exited = false;
  int child_status = 0;
  while (std::chrono::steady_clock::now() < deadline && !saw_readiness) {
    pollfd fds[] = {
        {.fd = stdout_pipe[0], .events = POLLIN, .revents = 0},
        {.fd = stderr_pipe[0], .events = POLLIN, .revents = 0},
    };
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    int ready = poll(fds, 2, static_cast<int>(remaining.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      fail_with_errno("poll");
    }
    if (ready == 0) {
      break;
    }
    if (fds[0].revents != 0) {
      stdout_output += read_available(stdout_pipe[0]);
      saw_readiness = stdout_output.contains('\n') &&
                      stdout_output.contains(R"("status":"ready")") &&
                      stdout_output.contains(R"("manifestPath")");
    }
    if (fds[1].revents != 0) {
      stderr_output += read_available(stderr_pipe[0]);
      if (stderr_output.size() > kMaxCapturedStderr) {
        stderr_output.erase(0, stderr_output.size() - kMaxCapturedStderr);
      }
    }
    pid_t wait_result = waitpid(pid, &child_status, WNOHANG);
    if (wait_result == pid) {
      child_exited = true;
      break;
    }
    if (wait_result < 0 && errno != EINTR) {
      fail_with_errno("waitpid");
    }
  }

  if (!saw_readiness) {
    if (!child_exited) {
      kill(pid, SIGKILL);
      waitpid(pid, &child_status, 0);
    }
    close_if_open(stdout_pipe[0]);
    close_if_open(stderr_pipe[0]);
    std::println(
        stderr,
        "{} waiting for hog-devnode readiness; status={}\nstdout:\n{}\n"
        "stderr tail:\n{}",
        child_exited ? "child exited before" : "timed out", child_status,
        stdout_output, stderr_output);
    return EXIT_FAILURE;
  }

  std::string manifest_path = json_string_field(stdout_output, "manifestPath");
  std::ifstream manifest_file(manifest_path);
  std::string manifest((std::istreambuf_iterator<char>(manifest_file)),
                       std::istreambuf_iterator<char>());
  if (!manifest_file || !manifest.contains(R"("chainState":"fresh")")) {
    cleanup_child(pid);
    close_if_open(stdout_pipe[0]);
    close_if_open(stderr_pipe[0]);
    std::println(stderr,
                 "fresh launch manifest did not report chainState=fresh: {}",
                 manifest);
    return EXIT_FAILURE;
  }

  cleanup_child(pid);
  close_if_open(stdout_pipe[0]);
  close_if_open(stderr_pipe[0]);
  return EXIT_SUCCESS;
}
// NOLINTEND(misc-include-cleaner)
