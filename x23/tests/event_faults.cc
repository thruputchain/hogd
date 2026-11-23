#include <atomic>
#include <cerrno>
#include <system_error>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <gtest/gtest.h>

#include "x23/event.h"
#include "x23/sockio.h"

using namespace x23;

namespace {

// Inject a single syscall failure; all subsequent calls use the real kernel.
std::atomic<int> create_error{}, eventfd_error{}, ctl_error{}, wait_error{};
std::atomic<int> ctl_operation{};

class ManualPoll : public CoroPoll {
public:
  explicit ManualPoll(ThreadPool &pool) : CoroPoll(pool, no_loop_thread{}) {}
  using CoroPoll::check;
};

} // namespace

extern "C" {
int __real_epoll_create1(int);
int __real_eventfd(unsigned, int);
int __real_epoll_ctl(int, int, int, epoll_event *);
int __real_epoll_wait(int, epoll_event *, int, int);

int
__wrap_epoll_create1(int flags)
{
  if (int err = create_error.exchange(0)) {
    errno = err;
    return -1;
  }
  return __real_epoll_create1(flags);
}

int
__wrap_eventfd(unsigned init, int flags)
{
  if (int err = eventfd_error.exchange(0)) {
    errno = err;
    return -1;
  }
  return __real_eventfd(init, flags);
}

int
__wrap_epoll_ctl(int epfd, int op, int fd, epoll_event *event)
{
  if (op == ctl_operation)
    if (int err = ctl_error.exchange(0)) {
      errno = err;
      return -1;
    }
  return __real_epoll_ctl(epfd, op, fd, event);
}

int
__wrap_epoll_wait(int epfd, epoll_event *events, int count, int timeout)
{
  if (int err = wait_error.exchange(0)) {
    errno = err;
    return -1;
  }
  return __real_epoll_wait(epfd, events, count, timeout);
}
}

TEST(EventFaults, ConstructorPropagatesSyscallFailures)
{
  ThreadPool pool(8);
  for (auto *fault : {&create_error, &eventfd_error, &ctl_error}) {
    ctl_operation = EPOLL_CTL_ADD;
    *fault = EMFILE;
    try {
      ManualPoll cp(pool);
      ADD_FAILURE() << "constructor ignored syscall failure";
    } catch (const std::system_error &err) {
      EXPECT_EQ(err.code().value(), EMFILE);
    }
    EXPECT_EQ(fault->load(), 0);
    ManualPoll recovered(pool);
    EXPECT_NO_THROW(recovered.check(0));
  }
}

TEST(EventFaults, PollInterruptionIsRetriedButOtherErrorsPropagate)
{
  ThreadPool pool(8);
  ManualPoll cp(pool);
  wait_error = EINTR;
  EXPECT_NO_THROW(cp.check(0));
  EXPECT_EQ(wait_error.load(), 0);
  wait_error = EBADF;
  try {
    cp.check(0);
    ADD_FAILURE() << "epoll_wait failure ignored";
  } catch (const std::system_error &err) {
    EXPECT_EQ(err.code().value(), EBADF);
  }
  EXPECT_NO_THROW(cp.check(0));
}

TEST(EventFaults, DeleteFailureStillCompletesPendingWait)
{
  ThreadPool pool(8);
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  UniqueFd reader(fds[0]), writer(fds[1]);
  set_nonblock(reader);
  ManualPoll cp(pool);
  for (bool poison : {false, true}) {
    auto pending = cp.read_wait(cp.read_clear(reader));
    ctl_operation = EPOLL_CTL_DEL;
    ctl_error = EIO;
    if (poison)
      cp.poison(reader, cp.generation(reader, false));
    else
      cp.reset(reader);
    EXPECT_EQ(ctl_error.load(), 0);
    EXPECT_EQ(pending(), EBADF);
    // The injected error left the kernel registration intact.
    // Clean it up before testing the next activation.
    cp.reset(reader);
    reader.reset();
    int next[2];
    ASSERT_EQ(pipe(next), 0);
    reader.reset(next[0]);
    writer.reset(next[1]);
    set_nonblock(reader);
  }
}
