
#include <atomic>
#include <cerrno>
#include <mutex>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "x23/async.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/fileio.h"

namespace x23 {

CoroPoll::CoroPoll(ThreadPool &pool)
  : pool_(pool), epollfd_(epoll_create1(EPOLL_CLOEXEC))
{
  if (!epollfd_)
    Errno{}.raise("epoll_create1 failed");
  std::lock_guard _g(resize_lock_); // Use lock to prevent race on st_
  pollthread_ = std::jthread(&CoroPoll::loop, this);
  st_ = pollthread_.get_stop_token();
}

CoroPoll::~CoroPoll()
{
  pollthread_.request_stop();
  wake();
  // join explicitly to avoid destroying any CoroPoll fields while
  // pollthread_ is still active.  (We could ensure this by having
  // pollthread_ as the last field, but that's not very robust.)
  pollthread_.join();
  destroy_wait();
  delete[] state_.load();
}

CoroPoll::FdPtr
CoroPoll::getfd(BorrowedFd fd)
{
  assert(fd.get() >= 0);

  if (size_t(fd.get()) >= state_size_) {
    ++want_resize_;
    Defer _c([this] { --want_resize_; });

    std::lock_guard _lg(resize_lock_);
    if (size_t(fd.get()) >= state_size_) {
      size_t new_size = std::max<size_t>(fd.get() + 1uz, 2 * state_size_);
      FdState *new_state = new FdState[new_size];
      std::move(state_.load(), state_ + state_size_, new_state);
      delete[] state_.exchange(new_state);
      state_size_ = new_size;
    }
  }
  else if (want_resize_)
    // Crude way to avoid writer starvation--briefly grab write lock
    (void)std::lock_guard{resize_lock_};

  std::shared_lock sl(resize_lock_);
  return FdPtr{state_ + fd.get(), std::move(sl)};
}

void
CoroPoll::check(int timeout)
{
  std::array<struct epoll_event, 128> evs;
  int n = epoll_wait(epollfd_.get(), evs.data(), evs.size(), timeout);
  if (n == -1) {
    if (errno == EINTR)
      return;
    Errno{}.raise("epoll_wait failed");
  }

  std::shared_lock lk(resize_lock_);
  std::vector<std::coroutine_handle<>> coroqueue;
  Errno err = st_.stop_requested() ? EBADF : 0;
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
  lk.unlock();

  pool_.go(coroqueue);
}

void
CoroPoll::loop()
{
  (void)std::lock_guard{resize_lock_}; // ensure st_ is initialized
  for (;;) {
    if (st_.stop_requested())
      if (numcoro_ == 0) {
        pool_.drain();
        if (numcoro_ == 0)
          return;
      }
    check();
  }
}

void
CoroPoll::clear(BorrowedFd fd, bool write)
{
  auto fdp = getfd(fd);
  assert(!fdp->callbacks[write].load(std::memory_order_relaxed));
  setmode(fdp, FdState::Mode::kActive);
  fdp->ready &= ~epflags[write];
}

void
CoroPoll::setcb(BorrowedFd fd, bool write, WorkItem wi)
{
  using enum FdState::Mode;
  FdPtr fdp = getfd(fd);
  FdState::Mode mode = fdp->mode;
  if (st_.stop_requested() || mode == kPoisoned) {
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
CoroPoll::setmode(const FdPtr &fdp, FdState::Mode mode)
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

Async<std::expected<size_t, Errno>>
CoroPoll::read(BorrowedFd fd, Buf *out, size_t len, bool full)
{
  size_t startsize = out->size();
  while (len > 0) {
    read_clear(fd);
    if (auto res = x23::read(fd, out, len)) {
      len -= *res;
      if (*res == 0 || !full)
        break;
    }
    else if (res.error() == EAGAIN) {
      if (Errno err = co_await read_wait(fd.get()))
        co_return err;
    }
    else
      co_return res;
  }
  co_return out->size() - startsize;
}

Async<std::expected<size_t, Errno>>
CoroPoll::write(BorrowedFd fd, BufView data)
{
  size_t len = data.size();
  while (!data.empty()) {
    write_clear(fd);
    auto n = ::write(fd.get(), data.data(), data.size());
    if (n > 0)
      data = data.subspan(n);
    else if (errno == EAGAIN) {
      if (Errno err = co_await write_wait(fd.get()))
        co_return err;
    }
    else
      co_return Errno{};
  }
  co_return len;
}

BorrowedFd
CoroPoll::always_eof()
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
    set_nonblock(newfd);
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
CoroPoll::wake()
{
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLONESHOT;
  ev.data.fd = always_eof().get();
  getfd(ev.data.fd); // make sure state_ is big enough in check()
  if (epoll_ctl(epollfd_.get(), EPOLL_CTL_ADD, ev.data.fd, &ev) == -1 &&
      errno != EEXIST) [[unlikely]]
    Errno{}.raise("EPOLL_CTL_ADD always_eof");
}

Async<std::expected<int, Errno>>
CoroPoll::tcpconnect(sockaddr *sa, socklen_t len)
{
  if (st_.stop_requested())
    co_return Errno{EBADF};
  UniqueFd fd{socket(sa->sa_family, SOCK_STREAM, 0)};
  if (!fd)
    co_return Errno{};
  set_nonblock(fd);
  write_clear(fd);
  if (::connect(fd.get(), sa, len) == -1) {
    Errno err{};
    if (err != EINPROGRESS)
      co_return err;
    if ((err = co_await write_wait(fd.get())))
      co_return err;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t len = sizeof(ss);
    if (getpeername(fd.get(), reinterpret_cast<struct sockaddr *>(&ss), &len))
      co_return Errno{};
  }
  co_return fd.release();
}

decltype(auto)
gai_category()
{
  struct gai_category_impl : public std::error_category {
    const char *name() const noexcept override { return "DNS"; }
    std::string message(int err) const override { return gai_strerror(err); }
  };
  static const gai_category_impl cat;
  return (cat);
}

Async<std::expected<int, std::system_error>>
CoroPoll::tcpconnect(std::string host, std::string port)
{
  if (st_.stop_requested())
    co_return std::unexpected{
        std::system_error{Errno{EBADF}, "CoroPoll shutting down"}};
  RefPtr _r(*this);

  ManagedPtr<freeaddrinfo> ai(nullptr);
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = AI_ADDRCONFIG;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int status = co_await pool_.async(getaddrinfo, host.c_str(), port.c_str(),
                                    &hints, &ai);
  if (status)
    co_return std::unexpected{std::system_error{status, gai_category(), host}};
  if (auto res = co_await tcpconnect(ai->ai_addr, ai->ai_addrlen))
    co_return *res;
  else
    co_return std::unexpected{std::system_error{res.error(), host}};
}

UniqueFd
tcplisten(std::uint16_t port)
{
  UniqueFd lfd(socket(AF_INET6, SOCK_STREAM, 0));
  if (!lfd)
    Errno{}.raise("socket");
  if (int flags = fcntl(lfd.get(), F_GETFD);
      flags == -1 || fcntl(lfd.get(), F_SETFD, flags | FD_CLOEXEC) == -1)
    Errno{}.raise("F_SETFD");

  int opt = 0;
  if (setsockopt(lfd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) < 0)
    Errno{}.raise("IPV6_V6ONLY");

  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(lfd.get(), reinterpret_cast<struct sockaddr *>(&addr),
           sizeof(addr)) < 0)
    Errno{}.raise("bind in6addr_any");

  if (listen(lfd.get(), 5) < 0)
    Errno{}.raise("listen");
  set_nonblock(lfd.get());
  return lfd;
}

} // namespace x23
