
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
};

CoroPollImpl::~CoroPollImpl()
{
  delete[] state_.load();
}

void
CoroPollImpl::clear(BorrowedFd fd, bool write)
{
  // std::println("{}_clear({})", write ? "write" : "read", fd.get());

  auto fdp = getfd(fd);
  assert(!fdp->callbacks[write].value.load(std::memory_order_relaxed));
  setmode(fdp, FdState::Mode::kActive);
  fdp->ready &= ~epflags[write] | epflags[!write];
}

void
CoroPollImpl::setcb(BorrowedFd fd, bool write, WorkItem wi)
{
  // std::println("setcb({}, {})", fd.get(), write ? "write" : "read");

  using enum FdState::Mode;
  FdPtr fdp = getfd(fd);
  FdState::Mode mode = fdp->mode();

  if (stop_requested() || mode == kPoisoned) {
    wi(EBADF);
    return;
  }

  // If this fails, you forgot to call {read,write}_clear before
  // calling {read,write}_wait.
  assert(mode != kFree);

  if ((fdp->ready & epflags[write]) && mode == kActive) {
    wi(0);
    return;
  }

  ++numcoro_;
  [[maybe_unused]] auto oldwi = fdp->callbacks[write].exchange(wi);
  // If this assertion fails, you attempted to register two read or
  // two write callbacks on the same file descriptor.
  assert(!*oldwi);

  setmode(fdp, kActive);
  // have to re-check in case write edge occured before we published wi
  docb(fdp, write);
}

void
CoroPollImpl::setmode(const FdPtr &fdp, FdState::Mode mode) noexcept
{
  using enum FdState::Mode;
  FdState::Mode old_mode = fdp->mode_;
  do {
    if (old_mode == mode || (mode == kActive && old_mode == kPoisoned))
      return;
    if (old_mode == kLocked) {
      fdp->mode_.wait(kLocked);
      continue;
    }
  } while (!fdp->mode_.compare_exchange_strong(old_mode, kLocked));

  if (mode == kActive && stop_requested()) {
    fdp->mode_ = old_mode;
    fdp->mode_.notify_all();
    return;
  }

  Defer unlock([&fdp, mode] {
    fdp->mode_ = mode;
    fdp->mode_.notify_all();
  });

  int fd = fdp.get() - state_;

  if (mode == kActive) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    [[maybe_unused]] int err =
        epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, fd, &ev);
    assert(err != -1);
  }
  else {
    fdp->ready = 0; // because EPOLLERR is sticky
    if (old_mode == kActive) {
      [[maybe_unused]] int err =
          epoll_ctl(epollfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
      assert(err != -1 || errno == EBADF || errno == ENOENT);
    }
    unlock.reset();
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

// To wake CoroPoll::check from another thread, we just add a
// level-triggered, one-shot check for a readable file descriptor
// (always true).  Because we don't update the active flags in state,
// check won't incur an epoll_ctl system call to remove the event.
void
CoroPollImpl::wake()
{
  uint64_t one{1};
  if (::write(eventfd_.get(), &one, sizeof(one)) == -1) [[unlikely]]
    Errno{}.raise("write(eventfd)");
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
    state_[fd].ready |= evs[i].events;
    for (unsigned write = 0; write < 2; ++write)
      if (evs[i].events & epflags[write]) {
        if (WorkItem wi = state_[fd].callbacks[write].exchange({})) {
          --numcoro_;
          if (std::coroutine_handle<> h = wi->set_result_without_resuming(err))
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
  if (WorkItem wi = getfd(fd.get())->callbacks[write].exchange({}))
    if (std::coroutine_handle<> h = wi->set_result_without_resuming(err))
      pool_.go({h, wi.cpuhint});
}

} // namespace x23::detail
