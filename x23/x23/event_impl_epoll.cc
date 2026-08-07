
#include <atomic>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "x23/event.h"
#include "x23/sockio.h"

namespace x23::detail {

CoroPollImpl::CoroPollImpl(ThreadPool &pool)
  : pool_(pool), epollfd_(epoll_create1(EPOLL_CLOEXEC))
{
  if (!epollfd_)
    Errno{}.raise("epoll_create1 failed");
  eventfd_.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  if (!eventfd_)
    Errno{}.raise("eventfd failed");
  set_nonblock(eventfd_);
  epoll_event ev{.events = EPOLLIN, .data = {.fd = eventfd_.get()}};
  if (epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, eventfd_.get(), &ev))
    Errno{}.raise("EPOLL_CTL_ADD(eventfd)");
  sigcb_ = signal_cb(SIGUSR1, [this] { debug_dump(); });
};

CoroPollImpl::~CoroPollImpl()
{
  delete[] state_.load();
}

void
CoroPollImpl::clear(BorrowedFd fd, bool write)
{
  // debug("{}_clear(fd {})", write ? "write" : "read", fd.get());

  auto fdp = getfd(fd);
  x23assert(!fdp->callbacks_[write].value.load(std::memory_order_relaxed));
  setmode(fdp, FdState::Mode::kActive);
  fdp->ready_ &= ~epflags[write] | epflags[!write];
}

void
CoroPollImpl::setcb(BorrowedFd fd, bool write, WorkItem wi)
{
  // debug("setcb({}, {})", fd.get(), write ? "write" : "read");

  using enum FdState::Mode;
  FdPtr fdp = getfd(fd);
  FdState::Mode mode = fdp->mode();
  if (stop_requested() || mode == kPoisoned) {
    wi(EBADF);
    return;
  }

  // If this fails, you forgot to call {read,write}_clear before
  // calling {read,write}_wait.
#if X23_FD_TRACE
  if (mode == kFree)
    panic("Mode was kFree when setting {} callback on fd {}\n{}", int(mode),
          write ? "write" : "read", fd.get(), *fdp->info_.take());
#else  // !X23_FD_TRACE
  x23assert(mode != kFree);
#endif // !X23_FD_TRACE

  if ((fdp->ready_ & epflags[write]) && mode == kActive) {
    wi(0);
    return;
  }

  ++numcoro_;
  [[maybe_unused]] auto oldwi = fdp->callbacks_[write].exchange(wi);
  // If this assertion fails, you attempted to register two read or
  // two write callbacks on the same file descriptor.
  x23assert(!*oldwi);

  setmode(fdp, kActive);
  // have to re-check in case write edge occurred before we published wi
  docb(fdp, write);
}

std::string
CoroPollImpl::showfd(const FdPtr &fdp)
{
  int fd = fdp.get() - state_;
  auto ret = std::format("  fd {}, mode {}, ready {}\n", fd,
                         int(fdp->mode_.load()), fdp->ready_.load());
  for (unsigned writer = 0; writer <= 1; ++writer) {
    if (auto cb = fdp->callbacks_[writer].value.load()) {
      uformat_to(&ret, "    {} (CPU {})\n", writer ? "writer" : "reader",
                 fdp->callbacks_[0].cpuhint.load());
      ret = trace(cb.resume_, 6, std::move(ret));
    }
  }
  return ret;
}

void
CoroPollImpl::debug_dump()
{
  using enum FdState::Mode;
  std::string msg;
  uformat_to(&msg, "x23::CoroPoll {}, eventfd {}\n",
             (void *)static_cast<CoroPoll *>(this), eventfd_.get());
  {
    std::unique_lock<SharedLock> lk;
    std::jthread lkthread;
    if (panicking()) {
      // By attempting to acquire the lock we will at least prevent
      // new read locks in the likely case that we can't acquire the
      // write lock because we already hold a read lock.  This could
      // lead to tracing deallocated coroutines, but we are panicking
      // anyway.
      lkthread =
          std::jthread([this, &lk] { lk = std::unique_lock(resize_lock_); });
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    else
      lk = std::unique_lock(resize_lock_);

    for (unsigned i = 0; i < state_size_; ++i) {
      FdState *p = state_ + i;
      if (p->mode_ == kFree)
        continue;
      msg += showfd(FdPtr{.p_ = p});
    }
  }
  int fd = debug.fd();
  if (fd < 0)
    fd = 2;
  ::write(fd, msg.data(), msg.size());
}

void
CoroPollImpl::setmode(const FdPtr &fdp, FdState::Mode mode) noexcept
{
  using enum FdState::Mode;

  FdState::Mode old_mode = fdp->mode_;
  if (mode == old_mode || (mode == kActive && old_mode == kPoisoned))
    return;

  while ((old_mode = fdp->mode_.exchange(kLocked)) == kLocked)
    fdp->mode_.wait(kLocked);

  int fd = fdp.get() - state_;
  FdState::Mode unlock_mode = old_mode;
  Defer unlock([&] {
    fdp->mode_ = unlock_mode;
    fdp->mode_.notify_all();
  });

  if (mode == old_mode ||
      (mode == kActive && (old_mode == kPoisoned || stop_requested())))
    return;

  unlock_mode = mode;
  fdp->generation_.fetch_add(1, std::memory_order_relaxed);

  if (mode == kActive) {
#if X23_FD_TRACE
    *fdp->info_.take() =
        std::format("fd {} activation stack trace\n{}", fd, stacktrace());
#endif // X23_FD_TRACE
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0)
      return;
    debug.err("EPOLL_CTL_ADD fd {}: {}", fd, Errno{});
    unlock_mode = mode = kPoisoned;
  }

  fdp->ready_ = 0; // because EPOLLERR is sticky
  if (old_mode == kActive) {
    if (epoll_ctl(epollfd_.get(), EPOLL_CTL_DEL, fd, nullptr) == -1)
      if (errno != ENOENT && errno != EBADF)
        debug.err("EPOLL_CTL_DEL fd {}: {}", fd, Errno{}, stacktrace());
  }
  unlock.reset();
  docbs(fdp);
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

// To wake CoroPoll::check from another thread, we write to an eventfd
// that we always monitor.
void
CoroPollImpl::wake()
{
  uint64_t one{1};
  if (::write(eventfd_.get(), &one, sizeof(one)) == -1) [[unlikely]]
    panic("write(eventfd): {}", Errno{});
}

CoroPollImpl::FdPtr
CoroPollImpl::getfd(BorrowedFd fd)
{
  x23assert(fd.get() >= 0);

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

RefPtr<ReadBuffer>
CoroPollImpl::get_reader(BorrowedFd fd)
{
  auto fdp = getfd(fd);
  if (fdp->mode() == FdState::kPoisoned)
    return nullptr;
  return fdp->reader();
}

RefPtr<WriteBuffer>
CoroPollImpl::get_writer(BorrowedFd fd)
{
  auto fdp = getfd(fd);
  if (fdp->mode() == FdState::kPoisoned)
    return nullptr;
  return fdp->writer();
}

std::pair<RefPtr<ReadBuffer>, RefPtr<WriteBuffer>>
CoroPollImpl::get_rw(BorrowedFd fd)
{
  std::pair<RefPtr<ReadBuffer>, RefPtr<WriteBuffer>> ret;
  auto fdp = getfd(fd);
  if (fdp->mode() != FdState::kPoisoned) {
    ret.first = fdp->reader();
    ret.second = fdp->writer();
  }
  return ret;
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
  std::vector<ThreadPool::Job> coroqueue;
  Errno err = stop_requested() ? EBADF : 0;
  for (int i = 0; i < n; ++i) {
    int fd = evs[i].data.fd;
    if (fd < 0)
      continue;
    if (fd == eventfd_.get()) {
      uint64_t buf;
      ::read(eventfd_.get(), &buf, sizeof(buf));
      continue;
    }
    state_[fd].ready_ |= evs[i].events;
    for (unsigned write = 0; write < 2; ++write)
      if (evs[i].events & epflags[write]) {
        if (WorkItem wi = state_[fd].callbacks_[write].exchange({})) {
          --numcoro_;
          if (CoroHandle h = wi->set_result_without_resuming(err))
            coroqueue.emplace_back(h, wi.cpuhint);
        }
      }
  }
  lk.reset();

  pool_.go(std::move(coroqueue));
}

void
CoroPollImpl::reset(BorrowedFd fd)
{
  auto fdp = getfd(fd);
  fdp->buf_clear();
#if X23_FD_TRACE
  *fdp->info_.take() =
      std::format("fd {} reset stack trace\n{}", fd.get(), stacktrace());
#endif // X23_FD_TRACE
  setmode(fdp, FdState::Mode::kFree);
}

void
CoroPollImpl::poison(BorrowedFd fd)
{
  auto fdp = getfd(fd);
#if X23_FD_TRACE
  *fdp->info_.take() =
      std::format("fd {} poison stack trace\n{}", fd.get(), stacktrace());
#endif // X23_FD_TRACE
  setmode(fdp, FdState::Mode::kPoisoned);
}

uint64_t
CoroPollImpl::generation(BorrowedFd fd, bool make)
{
  using enum FdState::Mode;
  auto fdp = getfd(fd);
  if (make && fdp->mode() == kFree && !stop_requested())
    setmode(fdp, kActive);
  return fdp->generation_.load(std::memory_order_relaxed);
}

void
CoroPollImpl::abort(BorrowedFd fd, bool write, int err)
{
  if (WorkItem wi = getfd(fd.get())->callbacks_[write].exchange({})) {
    --numcoro_;
    if (CoroHandle h = wi->set_result_without_resuming(err))
      pool_.go({h, wi.cpuhint});
  }
}

} // namespace x23::detail
