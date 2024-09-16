// -*- C++ -*-

#pragma once

#include <cstdint>
#include <shared_mutex>
#include <stop_token>
#include <sys/socket.h>
#include <thread>

#include <sys/epoll.h>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/mvar.h>
#include <x23/refcnt.h>
#include <x23/threadpool.h>
#include <x23/vec.h>

namespace x23 {

class CoroPoll : private DestroyInhibit {
public:
  using WorkItem = CoroCB<Errno>;
  ThreadPool &pool_;

  CoroPoll(ThreadPool &pool);
  ~CoroPoll();

  void read_clear(BorrowedFd fd) { clear(fd, false); }

  // Do a read_clear(fd) before a read operation on an fd that might
  // return EAGAIN.  If it does return EAGAIN, then co_await
  // read_wait(fd) to wait for the file descriptor to be readable
  // again.
  Async<Errno> read_wait(BorrowedFd fd)
  {
    auto [async, cb] = make_async<Errno>();
    setcb(fd, false, cb);
    return std::move(async);
  }

  void write_clear(BorrowedFd fd) { clear(fd, true); }

  // Do a write_clear(fd) before a write operation on an fd that might
  // return EAGAIN.  If it does return EAGAIN, then co_await
  // write_wait(fd) to wait for the file descriptor to be writable
  // again.
  Async<Errno> write_wait(BorrowedFd fd)
  {
    auto [async, cb] = make_async<Errno>();
    setcb(fd, true, cb);
    return std::move(async);
  }

  // You must reset a file descriptor before closing it if you have
  // ever called read_clear(), write_clear(), or poison() on it, even
  // if there are not any currently active callbacks.  This is
  // required to remove any epoll events.  If you don't call reset,
  // there could still be epoll events registered, or if you called
  // poison(), the file descriptor number could still be poisoned when
  // it is recycled by another socket.
  void reset(BorrowedFd fd) { setmode(getfd(fd), FdState::Mode::kFree); }

  // Make any pending and future callbacks return with EBADF untill
  // reset() is called.  Pending callbacks are made asynchronously, so
  // may still be running after poison() returns.
  void poison(BorrowedFd fd) { setmode(getfd(fd), FdState::Mode::kPoisoned); }

  // Read len bytes from fd and append them to *out.  If full is true,
  // will only read less than n bytes if there is an EOF.  Otherwise,
  // if full is false, it will return however many bytes it got from
  // the first successful call to read.
  Async<std::expected<size_t, Errno>> read(BorrowedFd fd, Buf *out, size_t len,
                                           bool full = true);

  // Write all of data to file descriptor fd.  The coroutine keeps
  // suspending itself until the entirety of data has been written or
  // an error occurs.  Always returns either data.size() or an Errno
  // other than EAGAIN.
  Async<std::expected<size_t, Errno>> write(BorrowedFd fd, BufView data);

  // Accept a new connection on a listening file descriptor lfd,
  // suspending the coroutine if no incoming connection is available.
  template<typename Addr = sockaddr>
  Async<std::expected<int, Errno>>
  accept(BorrowedFd lfd, Addr *addr = nullptr, socklen_t *addrlen = nullptr,
         int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
  {
    while (!st_.stop_requested()) {
      if constexpr (!std::same_as<decltype(addr), std::nullptr_t>)
        if (addrlen)
          *addrlen = sizeof(*addr);
      read_clear(lfd);
      int fd = accept4(lfd.get(), reinterpret_cast<sockaddr *>(addr), addrlen,
                       flags);
      if (fd >= 0)
        co_return fd;
      if (Errno err{}; err != EAGAIN)
        co_return err;
      if (auto err = co_await read_wait(lfd))
        co_return err;
    }
    co_return Errno{EBADF};
  }

  Async<std::expected<int, Errno>> tcpconnect(sockaddr *sa, socklen_t len);

  // Connect and return a file descriptor or a system error.
  Async<std::expected<int, std::system_error>> tcpconnect(std::string host,
                                                          std::string port);

  friend class RefPtr<CoroPoll>;

protected:
  void loop();
  void check(int timeout = -1);
  static BorrowedFd always_eof();
  void wake();

private:
  static inline constexpr std::uint32_t epflags[2] = {EPOLLIN, EPOLLOUT};

  struct FdState {
    enum Mode { kFree, kActive, kPoisoned };
    std::atomic<Mode> mode{kFree};
    std::atomic<std::uint32_t> ready{0};
    // callbacks: [0] = reader, [1] = writer
    std::array<std::atomic<WorkItem>, 2> callbacks{};

    FdState() noexcept = default;
    FdState(const FdState &other) { *this = other; }
    FdState &operator=(const FdState &other)
    {
      mode = other.mode.load();
      ready = other.ready.load();
      for (size_t i = 0; i < callbacks.size(); ++i)
        callbacks[i] = other.callbacks[i].load();
      return *this;
    }
  };

  UniqueFd epollfd_;

  std::shared_mutex resize_lock_;
  std::atomic<FdState *> state_{nullptr};
  std::atomic_size_t state_size_{0};
  std::atomic_uint want_resize_{0};
  std::atomic_uint numcoro_{0};

  std::stop_token st_;
  std::jthread pollthread_;

  struct FdPtr {
    FdState *p_;
    std::shared_lock<std::shared_mutex> lk_;
    FdState *get() const { return p_; }
    FdState *operator->() const { return p_; }
  };
  FdPtr getfd(BorrowedFd fd);

  void clear(BorrowedFd fd, bool write);
  void setcb(BorrowedFd fd, bool write, WorkItem h);
  void setmode(const FdPtr &fd, FdState::Mode mode);
  void docb(const FdPtr &fdp, bool write)
  {
    using enum FdState::Mode;
    FdState::Mode mode = fdp->mode;
    if ((fdp->ready & epflags[write]) || mode != kActive)
      if (WorkItem wi = fdp->callbacks[write].exchange(nullptr)) {
        --numcoro_;
        wi(mode == kActive ? 0 : EBADF);
      }
  }
  void docbs(const FdPtr &fdp)
  {
    docb(fdp, false);
    docb(fdp, true);
  }
};

void set_nonblock(BorrowedFd fd, bool on = true);

UniqueFd tcplisten(std::uint16_t port);

} // namespace x23
