#include <fcntl.h>
#include <sys/poll.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifndef HOGD_DEV_BINARY
#error "HOGD_DEV_BINARY must be defined by CMake"
#endif

namespace {

constexpr auto kStartupTimeout = std::chrono::seconds(30);
constexpr auto kShutdownTimeout = std::chrono::seconds(5);
constexpr std::size_t kMaxCapturedStderr = 256 * 1024;

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
  std::fprintf(stderr, "%.*s: %s\n", static_cast<int>(op.size()), op.data(),
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
terminate_child(pid_t pid)
{
  kill(pid, SIGTERM);
  int status = 0;
  if (!wait_for_exit(pid, kShutdownTimeout, status)) {
    kill(pid, SIGKILL);
    if (!wait_for_exit(pid, kShutdownTimeout, status)) {
      std::fprintf(stderr, "hogd-dev did not exit after SIGKILL\n");
      std::exit(EXIT_FAILURE);
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
    std::fprintf(stderr, "hogd-dev exited with status %d\n", status);
    std::exit(EXIT_FAILURE);
  }
}

} // namespace

int
main()
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

    std::vector<char *> argv{
        const_cast<char *>(HOGD_DEV_BINARY),
        const_cast<char *>("--listen-host"),
        const_cast<char *>("127.0.0.1"),
        const_cast<char *>("--scheduler-port"),
        const_cast<char *>("0"),
        const_cast<char *>("--readiness"),
        const_cast<char *>("stdout"),
        nullptr,
    };
    execv(HOGD_DEV_BINARY, argv.data());
    _exit(127);
  }

  close_if_open(stdout_pipe[1]);
  close_if_open(stderr_pipe[1]);
  set_nonblocking(stdout_pipe[0]);
  set_nonblocking(stderr_pipe[0]);

  std::string stdout_output;
  std::string stderr_output;
  const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
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
      saw_readiness =
          stdout_output.find('\n') != std::string::npos &&
          stdout_output.find("\"status\":\"ready\"") != std::string::npos &&
          stdout_output.find("\"manifestPath\"") != std::string::npos;
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
    std::fprintf(stderr,
                 "%s waiting for hogd-dev readiness; status=%d\nstdout:\n%s\n"
                 "stderr tail:\n%s\n",
                 child_exited ? "child exited before" : "timed out",
                 child_status, stdout_output.c_str(), stderr_output.c_str());
    return EXIT_FAILURE;
  }

  terminate_child(pid);
  close_if_open(stdout_pipe[0]);
  close_if_open(stderr_pipe[0]);
  return EXIT_SUCCESS;
}
