
#include "x23/event.h"
#include "x23/sockio.h"

namespace x23::detail {

CoroPollImpl::CoroPollImpl(ThreadPool &pool)
  : pool_(pool), epollfd_(epoll_create1(EPOLL_CLOEXEC))
{
  if (!epollfd_)
    Errno{}.raise("epoll_create1 failed");
}

CoroPollImpl::~CoroPollImpl()
{
  delete[] state_.load();
}

void
CoroPollImpl::clear(BorrowedFd fd, bool write)
{
  // std::println("{}_clear({})", write ? "write" : "read", fd.get());
  auto fdp = getfd(fd);
  assert(!fdp->callbacks[write].load(std::memory_order_relaxed));
  setmode(fdp, FdState::Mode::kActive);
  fdp->ready &= ~epflags[write];
}

void
CoroPollImpl::setcb(BorrowedFd fd, bool write, WorkItem wi)
{
  // std::println("setcb({}, {})", fd.get(), write ? "write" : "read");
  using enum FdState::Mode;
  FdPtr fdp = getfd(fd);
  FdState::Mode mode = fdp->mode();
  assert(mode != kFree);
  if (stop_requested() || mode == kPoisoned) {
    wi(EBADF);
    return;
  }
  if ((fdp->ready & epflags[write]) && mode == kActive) {
    wi(0);
    return;
  }

  ++numcoro_;
  [[maybe_unused]] auto oldwi = fdp->callbacks[write].exchange(wi);
  // If this assertion fails, you attempted to register two read or
  // two write callbacks on the same file descriptor.
  assert(!oldwi);

  setmode(fdp, kActive);
  // have to re-check in case write edge occured before we published wi
  docb(fdp, write);
}

void
CoroPollImpl::setmode(const FdPtr &fdp, FdState::Mode mode)
{
  using enum FdState::Mode;
  FdState::Mode old_mode = fdp->mode_;
  if (mode == old_mode)
    return;

  if ((mode == kActive && old_mode == kPoisoned) ||
      !fdp->mode_.compare_exchange_strong(old_mode, kLocked))
    return;
  Defer unlock([&fdp] { fdp->mode_ = kPoisoned; });

  int fd = fdp.get() - state_;

  if (mode == kActive) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, fd, &ev) == -1)
      Errno{}.raise("EPOLL_CTL_ADD fd {} failed", fd);
    fdp->mode_ = mode;
    unlock.release();
  }
  else {
    if (old_mode == kActive &&
        epoll_ctl(epollfd_.get(), EPOLL_CTL_DEL, fd, nullptr) == -1
	&& errno != EBADF)
      Errno{}.raise("EPOLL_CTL_DEL fd {} failed", fd);
    fdp->mode_ = mode;
    unlock.release();
    docbs(fdp);
  }
}

bool
CoroPollImpl::time_to_stop()
{
  if (!stop_requested() || numcoro_)
    return false;
  pool_.drain();
  return !numcoro_;
}

int
CoroPollImpl::get_max_fd()
{
  return state_size_;
}

BorrowedFd
CoroPollImpl::always_eof()
{
  // Note: /dev/null doesn't support epoll, so use a half-closed pipe
  // as a file descriptor that always returns EOF.
  static constinit std::atomic_int fd{-1};
  int val = fd.load(std::memory_order_relaxed);
  if (val == -1) [[unlikely]] {
    int fds[2];
    if (pipe(fds) == -1)
      Errno{}.raise("pipe");
    close(fds[1]);
    int newfd = fds[0];
    try {
      set_nonblock(newfd);
    } catch (...) {
      close(newfd);
      throw;
    }
    if (fd.compare_exchange_strong(val, newfd))
      return newfd;
    close(newfd);
  }
  return val;
}

// To wake CoroPoll::check from another thread, we just add a
// level-triggered, one-shot check for a readable file descriptor
// (always true).  Because we don't update the active flags in state,
// check won't incur an epoll_ctl system call to remove the event.
void
CoroPollImpl::wake()
{
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLONESHOT;
  ev.data.fd = always_eof().get();
  getfd(ev.data.fd); // make sure state_ is big enough in check()
  if (epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, ev.data.fd, &ev) == -1 &&
      errno != EEXIST) [[unlikely]]
    Errno{}.raise("EPOLL_CTL_ADD always_eof");
}

CoroPollImpl::FdPtr
CoroPollImpl::getfd(BorrowedFd fd)
{
  assert(fd.get() >= 0);

  if (size_t(fd.get()) >= state_size_) {
    std::lock_guard _lg(resize_lock_);
    if (size_t(fd.get()) >= state_size_) {
      size_t new_size = std::max<size_t>(fd.get() + 1UZ, 2 * state_size_);
      auto *new_state = new FdState[new_size];
      std::move(state_.load(), state_ + state_size_, new_state);
      delete[] state_.exchange(new_state);
      state_size_ = new_size;
    }
  }

  auto sl = resize_lock_.hold_shared();
  return FdPtr{.p_ = state_ + fd.get(), .lk_ = std::move(sl)};
}

std::shared_ptr<ReadBuffer>
CoroPollImpl::get_reader(BorrowedFd fd)
{
  auto fdp = getfd(fd);
  if (fdp->mode() == FdState::kPoisoned)
    return nullptr;
  return getfd(fd)->reader();
}

std::shared_ptr<WriteBuffer>
CoroPollImpl::get_writer(BorrowedFd fd)
{
  auto fdp = getfd(fd);
  if (fdp->mode() == FdState::kPoisoned)
    return nullptr;
  return getfd(fd)->writer();
}

void
CoroPollImpl::check(int timeout)
{
  std::array<struct epoll_event, 128> evs;
  int n = epoll_wait(epollfd_.get(), evs.data(), evs.size(), timeout);
  if (n == -1) {
    if (errno == EINTR)
      return;
    Errno{}.raise("epoll_wait failed");
  }

  auto lk = resize_lock_.hold_shared();
  std::vector<std::coroutine_handle<>> coroqueue;
  Errno err = stop_requested() ? EBADF : 0;
  for (int i = 0; i < n; ++i) {
    int fd = evs[i].data.fd;
    state_[fd].ready |= evs[i].events;
    for (unsigned write = 0; write < 2; ++write)
      if (evs[i].events & epflags[write]) {
        if (WorkItem wi = state_[fd].callbacks[write].exchange(nullptr)) {
          --numcoro_;
          if (std::coroutine_handle<> h = wi.set_result_without_resuming(err))
            coroqueue.push_back(h);
        }
      }
  }
  lk.reset();

  pool_.go(coroqueue);
}

void
CoroPollImpl::reset(BorrowedFd fd)
{
  // std::println("reset({})", fd.get());
  auto fdp = getfd(fd);
  fdp->buf_clear();
  setmode(fdp, FdState::Mode::kFree);
}

void
CoroPollImpl::poison(BorrowedFd fd)
{
  setmode(getfd(fd), FdState::Mode::kPoisoned);
}

void
CoroPollImpl::abort(BorrowedFd fd, bool write, int err)
{
  if (WorkItem wi = getfd(fd.get())->callbacks[write].exchange(nullptr))
    if (std::coroutine_handle<> h = wi.set_result_without_resuming(err))
      pool_.go(h);
}

} // namespace x23::detail
