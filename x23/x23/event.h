// -*- C++ -*-

#pragma once

#include <cstdint>
#include <stop_token>

#include <sys/socket.h>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/config.h>
#include <x23/corosync.h>
#include <x23/err.h>
#include <x23/fileio.h>
#include <x23/growvec.h>
#include <x23/mvar.h>
#include <x23/refcnt.h>
#include <x23/testlock.h>
#include <x23/threadpool.h>
#include <x23/vec.h>

#if USE_EPOLL
#include <sys/epoll.h>
#else // !USE_EPOLL
#include <poll.h>
#include <unordered_map>
#endif // !USE_EPOLL

namespace x23 {

namespace detail {

struct BufferedReader : std::enable_shared_from_this<BufferedReader> {
  using Super = std::enable_shared_from_this<BufferedReader>;
  using Mem = std::array<char, 4092 - sizeof(Super)>;

  std::uint16_t start_{};
  std::uint16_t stop_{};
  Mem m_;

  ssize_t operator()(int fd, void *buf, size_t len);
  std::expected<size_t, Errno> read(BorrowedFd fd, is_bytevec auto *buf,
                                    size_t len)
  {
    return x23::read(fd, buf, len, this);
  }
};

struct BufHolder {
  TestLock buflk_;
  std::shared_ptr<BufferedReader> reader_;

  std::shared_ptr<detail::BufferedReader> reader()
  {
    std::lock_guard _g(buflk_);
    if (!reader_)
      reader_ = std::make_shared<detail::BufferedReader>();
    return reader_;
  }
  void buf_clear()
  {
    std::lock_guard _g(buflk_);
    reader_.reset();
  }
};

class CoroPollImpl : public DestroyInhibit {
public:
  using WorkItem = CoroCB<Errno>;
  ThreadPool &pool_;

  CoroPollImpl(ThreadPool &pool);
  ~CoroPollImpl();

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
  void reset(BorrowedFd fd);

  // Make any pending and future callbacks return with EBADF untill
  // reset() is called.  Pending callbacks are made asynchronously, so
  // may still be running after poison() returns.
  void poison(BorrowedFd fd);

  // If there's a callback for reading (write == false) or writing
  // (write == true) on file descriptor fd, then immediately make the
  // callback with condition err, even though the awaited condition
  // does not hold yet.
  void abort(BorrowedFd fd, bool write, int err = ETIMEDOUT);

protected:
  void check(int timeout = -1);
  bool stop_requested() { return ss_.stop_requested(); }
  void request_stop() { ss_.request_stop(); }
  bool time_to_stop();
  int get_max_fd();
  void wake();
  std::shared_ptr<BufferedReader> get_reader(BorrowedFd fd);

private:
  std::stop_source ss_;
  void clear(BorrowedFd fd, bool write);
  void setcb(BorrowedFd fd, bool write, WorkItem h);

#if USE_EPOLL
  static inline constexpr std::uint32_t epflags[2] = {EPOLLIN | EPOLLHUP |
                                                          EPOLLERR,
                                                      EPOLLOUT | EPOLLERR};

  struct FdState : BufHolder {
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

  SharedLock resize_lock_{std::thread::hardware_concurrency()};
  std::atomic<FdState *> state_{nullptr};
  std::atomic_size_t state_size_{0};
  std::atomic_uint numcoro_{0};

  std::stop_token st_;

  struct FdPtr {
    FdState *p_;
    SharedLock::SharedHolder lk_;
    FdState *get() const { return p_; }
    FdState *operator->() const { return p_; }
  };
  FdPtr getfd(BorrowedFd fd);

  static BorrowedFd always_eof();
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

#else  // !USE_EPOLL
  static inline constexpr short pollflags[2] = {POLLIN | POLLHUP | POLLERR,
                                                POLLOUT | POLLERR};
  struct FdState : BufHolder {
    bool poisoned{false};
    std::array<WorkItem, 2> callbacks;
    short events() const
    {
      short ret = 0;
      for (size_t write = 0; write < 2; ++write)
        if (callbacks[write])
          ret |= pollflags[write];
      return ret;
    }
  };
  MVar<std::unordered_map<int, FdState>> state_;
  UniqueFd selfpipe_[2];

  std::vector<std::coroutine_handle<>> report_badfd(FdState &fds);
#endif // !USE_EPOLL
};

} // namespace detail

class CoroPoll : public detail::CoroPollImpl {
public:
  CoroPoll(ThreadPool &pool);
  ~CoroPoll();

  // Read len bytes from fd and append them to *out.  If full is true,
  // will only read less than n bytes if there is an EOF.  Otherwise,
  // if full is false, it will return however many bytes it got from
  // the first successful call to read.
  Async<std::expected<size_t, Errno>> read(BorrowedFd fd, Buf *out, size_t len,
                                           bool full = true SLOC_PARM);

  // Write all of data to file descriptor fd.  The coroutine keeps
  // suspending itself until the entirety of data has been written or
  // an error occurs.  Always returns either data.size() or an Errno
  // other than EAGAIN.
  Async<std::expected<size_t, Errno>> write(BorrowedFd fd,
					    BufView data SLOC_PARM);

  // Accept a new connection on a listening file descriptor lfd,
  // suspending the coroutine if no incoming connection is available.
  template<typename Addr = sockaddr>
  Async<std::expected<int, Errno>> accept(BorrowedFd lfd, Addr *addr = nullptr,
                                          socklen_t *addrlen = nullptr,
                                          bool close_on_exec = true SLOC_PARM)
  {
    while (!stop_requested()) {
      if constexpr (!std::same_as<decltype(addr), std::nullptr_t>)
        if (addrlen)
          *addrlen = sizeof(*addr);
      read_clear(lfd);
#if HAVE_ACCEPT4
      int fd =
          accept4(lfd.get(), reinterpret_cast<sockaddr *>(addr), addrlen,
                  close_on_exec ? SOCK_NONBLOCK | SOCK_CLOEXEC : SOCK_NONBLOCK);
#else  // !HAVE_ACCEPT4
      int fd = ::accept(lfd.get(), reinterpret_cast<sockaddr *>(addr), addrlen);
      if (fd >= 0) {
        try {
          set_nonblock(fd);
        } catch (...) {
          close(fd);
          throw;
        }
        if (close_on_exec)
          fcntl(fd, F_SETFD, 1);
      }
#endif // !HAVE_ACCEPT4
      if (fd >= 0)
        co_return fd;
      if (Errno err{}; err != EAGAIN)
        co_return err;
      if (auto err = co_await read_wait(lfd))
        co_return err;
    }
    co_return Errno{EBADF};
  }

  Async<std::expected<int, Errno>> tcpconnect(const sockaddr *sa,
                                              socklen_t len);
  Async<std::expected<int, Errno>> tcpconnect(const sockaddr_storage *ss)
  {
    return tcpconnect(reinterpret_cast<const sockaddr *>(ss), sizeof(*ss));
  }

  // Connect and return a file descriptor or a system error.
  Async<std::expected<int, std::system_error>> tcpconnect(std::string host,
                                                          std::string port);

private:
  std::thread pollthread_;

  void loop();
};

} // namespace x23
