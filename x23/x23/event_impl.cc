
#include "x23/event.h"
#include "x23/sockio.h"

namespace x23::detail {

#if HAVE_EPOLL

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
  auto fdp = getfd(fd);
  assert(!fdp->callbacks[write].load(std::memory_order_relaxed));
  setmode(fdp, FdState::Mode::kActive);
  fdp->ready &= ~epflags[write];
}

void
CoroPollImpl::setcb(BorrowedFd fd, bool write, WorkItem wi)
{
  using enum FdState::Mode;
  FdPtr fdp = getfd(fd);
  FdState::Mode mode = fdp->mode;
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
  // have to re-check in case write edge occured after we published wi
  docb(fdp, write);
}

void
CoroPollImpl::setmode(const FdPtr &fdp, FdState::Mode mode)
{
  using enum FdState::Mode;
  FdState::Mode old_mode = fdp->mode;
  if (mode == old_mode || (mode == kActive && old_mode == kPoisoned) ||
      !fdp->mode.compare_exchange_strong(old_mode, mode))
    return;

  int fd = fdp.get() - state_;
  if (mode == kActive) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, fd, &ev) == -1)
      Errno{}.raise("EPOLL_CTL_ADD failed");
    // Might have missed an edge, before epoll_ctl, so set all flags
    fdp->ready = EPOLLIN | EPOLLOUT;
  }
  else {
    if (old_mode == kActive &&
        epoll_ctl(epollfd_.get(), EPOLL_CTL_DEL, fd, nullptr) == -1)
      Errno{}.raise("EPOLL_CTL_DEL failed");
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

std::shared_ptr<BufferedReader>
CoroPollImpl::get_reader(BorrowedFd fd)
{
  auto fdp = getfd(fd);
  if (fdp->mode == FdState::kPoisoned)
    return nullptr;
  return getfd(fd)->reader();
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

#else // !HAVE_EPOLL

CoroPollImpl::CoroPollImpl(ThreadPool &pool_) : pool_(pool_)
{
  int fds[2];
  if (pipe(fds))
    Errno{}.raise("pipe");
  for (int i = 0; i < 2; ++i)
    selfpipe_[i].reset(fds[i]);
  // Don't combine loops; set_nonblock might throw.
  for (int i = 0; i < 2; ++i)
    set_nonblock(selfpipe_[i].get());
}

CoroPollImpl::~CoroPollImpl()
{}

bool
CoroPollImpl::time_to_stop()
{
  if (!stop_requested() || !state_->empty())
    return false;
  pool_.drain();
  return state_->empty();
}

std::shared_ptr<BufferedReader>
CoroPollImpl::get_reader(BorrowedFd fd)
{
  auto s = state_.take();
  auto &f = (*s)[fd.get()];
  if (f.poisoned)
    return nullptr;
  return f.reader();
}

void
CoroPollImpl::check(int timeout)
{
  std::vector<struct pollfd> pollfds;
  {
    auto ptr = state_.take();
    pollfds.reserve(ptr->size() + 1);
    pollfds.push_back({selfpipe_[0].get(), pollflags[0], 0});
    for (const auto &kv : *ptr) {
      const FdState &f = kv.second;
      if (f.poisoned)
        continue;
      pollfds.push_back({kv.first, f.events(), 0});
    }
  }

  int n = poll(pollfds.data(), pollfds.size(), timeout);
  if (n <= 0) {
    if (n < 0 && errno != EINTR)
      Errno{}.raise("poll");
    return;
  }
  if (pollfds[0].revents) {
    char buf[64];
    while (::read(selfpipe_[0].get(), &buf, sizeof(buf)) == sizeof(buf))
      ;
    if (!--n)
      return;
  }

  auto ptr = state_.take();
  std::vector<std::coroutine_handle<>> cbs;
  for (auto pfd = pollfds.begin() + 1; pfd != pollfds.end(); ++pfd) {
    auto it = ptr->find(pfd->fd);
    if (it == ptr->end())
      continue;
    FdState &f = it->second;
    if (f.poisoned)
      continue;
    for (int write = 0; write < 2; ++write)
      if (f.callbacks[write] && (pfd->revents & pollflags[write]))
        if (auto h = std::exchange(f.callbacks[write], nullptr)
                         .set_result_without_resuming(0))
          cbs.push_back(h);
    if (!f.callbacks[0] && !f.callbacks[1])
      ptr->erase(it);
  }
  ptr.reset();
  pool_.go(cbs);
}

int
CoroPollImpl::get_max_fd()
{
  auto ptr = state_.take();
  if (ptr->empty())
    return 0;
  return 1 + std::ranges::max(*ptr | std::views::keys);
}

std::vector<std::coroutine_handle<>>
CoroPollImpl::report_badfd(FdState &f)
{
  std::vector<std::coroutine_handle<>> ret;
  for (unsigned write = 0; write < 2; ++write)
    if (f.callbacks[write])
      if (auto h = std::exchange(f.callbacks[write], nullptr)
                       .set_result_without_resuming(EBADF))
        ret.push_back(h);
  return ret;
}

void
CoroPollImpl::poison(BorrowedFd fd)
{
  auto s = state_.take();
  auto &f = (*s)[fd.get()];
  f.poisoned = true;
  auto work = report_badfd(f);
  s.reset();
  pool_.go(work);
}

void
CoroPollImpl::reset(BorrowedFd fd)
{
  auto ptr = state_.take();
  if (auto it = ptr->find(fd.get()); it != ptr->end()) {
    auto work = report_badfd(it->second);
    ptr->erase(it);
    if (ptr->empty() && stop_requested())
      wake();
    ptr.reset();
    pool_.go(work);
  }
}

void
CoroPollImpl::clear(BorrowedFd fd, bool)
{}

void
CoroPollImpl::setcb(BorrowedFd fd, bool write, WorkItem cb)
{
  if (stop_requested()) {
    pool_.make_callback(cb, EBADF);
    return;
  }
  auto ptr = state_.take();
  auto &f = (*ptr)[fd.get()];
  assert(!f.callbacks[write]);
  if (f.poisoned) {
    pool_.make_callback(cb, EBADF);
    return;
  }
  f.callbacks[write] = cb;
  wake();
}

void
CoroPollImpl::wake()
{
  char z = '\0';
  write(selfpipe_[1].get(), &z, 1);
}

void
CoroPollImpl::abort(BorrowedFd fd, bool write, int err)
{
  std::coroutine_handle<> h{nullptr};
  {
    auto ptr = state_.take();
    auto &f = (*ptr)[fd.get()];
    if (f.callbacks[write])
      h = std::exchange(f.callbacks[write], nullptr)
              .set_result_without_resuming(err);
  }
  if (h)
    pool_.go(h);
}

#endif // !HAVE_EPOLL

} // namespace x23::detail
