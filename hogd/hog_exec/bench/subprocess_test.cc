// What a stopped benchmark process leaves behind.
//
// The control server's [stop] is what stands between two benchmark suites: the
// next suite binds the same fixed ports seconds later. A process the suite
// started that is still running then -- chuckd's scpd holding 127.0.0.1:4400
// is the case that broke CI -- makes the next suite's chuckd fail to bind and
// panic out of startup. So these pin that stopping a process stops the
// processes it started, whether or not the process itself is still alive to be
// signalled.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <signal.h> // NOLINT(modernize-deprecated-headers)
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/log.h"
#include "hogd/hog_exec/bench/subprocess.h"

namespace hogd {
namespace {

// Short enough to keep the tests quick, long enough that a loaded machine
// still gets a process scheduled in between.
constexpr std::chrono::milliseconds kTermGrace{2000};
constexpr std::chrono::milliseconds kKillGrace{2000};
constexpr std::chrono::milliseconds kSpawnTimeout{10000};
constexpr std::chrono::milliseconds kExitTimeout{10000};
constexpr std::chrono::milliseconds kPollInterval{20};

class SubprocessTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    pid_path_ = std::filesystem::temp_directory_path() /
                ("subprocess_test_" + std::to_string(getpid()) + "_" +
                 std::to_string(++counter_) + ".pid");
    std::filesystem::remove(pid_path_);
  }

  void TearDown() override
  {
    if (helper_pid_ > 0) {
      kill(helper_pid_, SIGKILL);
    }
    std::filesystem::remove(pid_path_);
  }

  // Runs [script] under /bin/sh. The script is expected to write the pid of a
  // process it starts to $PIDFILE; [helper_pid] is that pid, once it appears.
  void launch_script(Subprocess &proc, std::string const &script)
  {
    std::vector<std::string> args = {"-c", "PIDFILE=" + pid_path_.string() +
                                               "\nexport PIDFILE\n" + script};
    auto pid = proc.launch_process("/bin/sh", args, "");
    ASSERT_TRUE(pid.has_value());
    launched_pid_ = pid.value_or(-1);
  }

  // Waits for the script to report the pid of the process it started. Waits
  // for the pid to be signallable too: a pid in the file is not yet a process
  // this test may assert about being alive.
  // NOLINTNEXTLINE(misc-include-cleaner)
  pid_t await_helper_pid()
  {
    auto deadline = std::chrono::steady_clock::now() + kSpawnTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      std::ifstream in(pid_path_);
      pid_t pid = 0;
      if (in >> pid && pid > 0 && kill(pid, 0) == 0) {
        helper_pid_ = pid;
        return pid;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    ADD_FAILURE() << "helper process never reported its pid";
    return -1;
  }

  // Whether [pid] is running right now. A process that has exited but has not
  // yet been reaped still answers to kill(pid, 0), and these tests are about
  // processes that are still running.
  static bool process_running(pid_t pid)
  {
    if (kill(pid, 0) == -1) {
      return false;
    }
    auto state = detail::read_proc_state(pid);
    return !state || state->state != 'Z';
  }

  // Waits out the gap between a process being signalled and the kernel
  // agreeing it is gone. A process the stop failed to reach never goes.
  static bool await_process_gone(pid_t pid)
  {
    auto deadline = std::chrono::steady_clock::now() + kExitTimeout;
    while (process_running(pid)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    return true;
  }

  std::filesystem::path pid_path_;
  pid_t launched_pid_ = -1;
  pid_t helper_pid_ = -1;
  static inline int counter_ = 0;
};

// The shape that broke CI: the launched process starts a helper and is then
// replaced by exec, so the helper is the launched process's child and outlives
// it. Signalling the launched process alone never reaches the helper.
TEST_F(SubprocessTest, StopReachesAProcessTheChildStarted)
{
  Subprocess proc;
  ASSERT_NO_FATAL_FAILURE(launch_script(proc, R"(
sleep 300 &
echo $! > "$PIDFILE"
exec sleep 300
)"));

  pid_t helper = await_helper_pid();
  ASSERT_GT(helper, 0);
  EXPECT_NE(helper, launched_pid_);

  proc.terminate();
  proc.wait_for_shutdown();
  EXPECT_TRUE(proc.reap(kTermGrace, kKillGrace));

  EXPECT_TRUE(await_process_gone(helper))
      << "a process started by the stopped process is still running";
}

// A helper that has already outlived the process that started it is still the
// stop's to clean up -- there is no one else left to do it.
TEST_F(SubprocessTest, StopReachesAnOrphanedProcess)
{
  Subprocess proc;
  ASSERT_NO_FATAL_FAILURE(launch_script(proc, R"(
sleep 300 &
echo $! > "$PIDFILE"
exit 0
)"));

  pid_t helper = await_helper_pid();
  ASSERT_GT(helper, 0);

  // Let the launched process exit, orphaning the helper, before stopping.
  auto deadline = std::chrono::steady_clock::now() + kSpawnTimeout;
  while (!proc.get_exit_code().has_value() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(kPollInterval);
  }
  ASSERT_TRUE(proc.get_exit_code().has_value());
  ASSERT_TRUE(process_running(helper));

  proc.terminate();
  proc.wait_for_shutdown();
  EXPECT_TRUE(proc.reap(kTermGrace, kKillGrace));

  EXPECT_TRUE(await_process_gone(helper))
      << "an orphaned process was left running by stop";
}

// SIGTERM is a request. A helper that declines it still has to be gone by the
// time stop returns, because the next suite binds its ports immediately.
TEST_F(SubprocessTest, StopEscalatesToSigkill)
{
  Subprocess proc;
  ASSERT_NO_FATAL_FAILURE(launch_script(proc, R"(
/bin/sh -c 'trap "" TERM; echo $$ > "$PIDFILE"; while :; do sleep 1; done' &
exec sleep 300
)"));

  pid_t helper = await_helper_pid();
  ASSERT_GT(helper, 0);

  proc.terminate();
  proc.wait_for_shutdown();
  EXPECT_TRUE(proc.reap(kTermGrace, kKillGrace));

  EXPECT_TRUE(await_process_gone(helper))
      << "a process that ignored SIGTERM outlived stop";
}

// The exit code of the launched process is what callers read to decide whether
// a benchmark step passed; grouping and reaping must not disturb it.
TEST_F(SubprocessTest, ReportsTheLaunchedProcessExitCode)
{
  Subprocess proc;
  std::vector<std::string> args = {"-c", "exit 7"};
  auto pid = proc.launch_process("/bin/sh", args, "");
  ASSERT_TRUE(pid.has_value());

  proc.wait_for_shutdown();
  auto exit_code = proc.get_exit_code();
  ASSERT_TRUE(exit_code.has_value());
  EXPECT_EQ(exit_code.value_or(-1), 7);

  // Nothing else was started, so the reap has nothing to wait for.
  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(proc.reap(kTermGrace, kKillGrace));
  EXPECT_LT(std::chrono::steady_clock::now() - start, kTermGrace);
}

} // namespace
} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  hogd::log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
