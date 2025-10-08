
#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "x23/async.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

#include <thread>

namespace x23 {

namespace detail {

Errno
WriteBuffer::Service::write(BufVec &work)
{
  cp_->write_clear(fd_);
  auto res = work.write(fd_);
  got_eagain_ = res && *res == IoResult::kAgain;
  if (!res)
    cp_->poison(fd_);
  return 0;
}

Errno
WriteBuffer::Service::go()
{
  wb_->want_write_ = true;
  if (!lk_.try_lock())
    return 0;
  BufVec work = get_work();
  // Try writing once before allocating coroutine
  if (work.empty() && !work_needed())
    return 0;
  if (auto err = write(work))
    return err;
  if (!work.empty() || work_needed())
    worker(std::move(*this), std::move(work));
  return 0;
}

Detached
WriteBuffer::Service::worker(Service self, BufVec work)
{
  RefPtr<WriteBuffer> wbref(*self.wb_);
  for (;;) {
    while (!work.empty()) {
      if (self.got_eagain_)
        co_await self.cp_->write_wait(self.fd_);
      if (self.write(work))
        co_return;
    }

    if (!self.work_needed())
      co_return;
    work = self.get_work();
  }
}

} // namespace detail

CoroPoll::CoroPoll(ThreadPool &pool) : CoroPoll(pool, no_loop_thread{})
{
  pollthread_ = std::thread(&CoroPoll::loop, this);
}

CoroPoll::CoroPoll(ThreadPool &pool, no_loop_thread) : CoroPollImpl(pool)
{
  if (ignore_sigpipe) {
    ignore_sigpipe = false;
    std::signal(SIGPIPE, SIG_IGN);
  }
}

CoroPoll::~CoroPoll()
{
  request_stop();
  for (int i = 0, e = get_max_fd(); i < e; ++i)
    reset(i);
  wake();
  if (pollthread_.joinable())
    pollthread_.join();
  destroy_wait();
}

void
CoroPoll::loop()
{
  while (!time_to_stop())
    check();
}

Async<std::expected<size_t, Errno>>
CoroPoll::read(BorrowedFd fd, Buf *out, size_t len, bool full,
               bool sync SLOC_ARG)
{
  if (stop_requested())
    co_return Errno{EBADF};
  size_t startsize = out->size();
  while (len > 0) {
    read_clear(fd);
    RefPtr<detail::ReadBuffer> r{nullptr};
    RefPtr<detail::WriteBuffer> w{nullptr};
    if (sync)
      std::tie(r, w) = get_rw(fd);
    else
      r = get_reader(fd);
    if (!r)
      co_return Errno{EBADF};
    Defer flush;
    if (sync && len > r->nbuffered())
      flush.reset([this, &w, &fd] { w->flush(*this, fd); });
    if (auto res = r->read(fd, out, len)) {
      len -= *res;
      if (*res == 0 || !full)
        break;
    }
    else if (res.error() == EAGAIN) {
      if (sync) {
        w->waiting_for_read_ = true;
        // flush now before going to sleep
        flush.reset([&w] { w->waiting_for_read_ = false; });
      }
      if (Errno err = co_await read_wait(fd))
        co_return err;
    }
    else
      co_return res;
  }
  co_return out->size() - startsize;
}

Async<std::expected<size_t, Errno>>
CoroPoll::write(BorrowedFd fd, BufView data SLOC_ARG)
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

Errno
CoroPoll::awrite(BorrowedFd fd, Buf data)
{
  auto w = get_writer(fd);
  if (!w)
    return EBADF;
  return w->write(*this, fd, std::move(data));
}

Errno
CoroPoll::dwrite(BorrowedFd fd, Buf data)
{
  auto w = get_writer(fd);
  if (!w)
    return EBADF;
  if (Errno err = w->buffer(*this, fd, std::move(data)))
    return err;
  if (w->waiting_for_read_)
    return w->flush(*this, fd);
  return 0;
}

Async<std::expected<void, Errno>>
CoroPoll::connect(BorrowedFd fd, const sockaddr *sa, socklen_t len SLOC_ARG)
{
  if (stop_requested())
    co_return Errno{EBADF};
  set_nonblock(fd);
  set_nodelay(fd);

  int err = 0;
  reset(fd);
  write_clear(fd);
  if (::connect(fd.get(), sa, len) == -1)
    err = errno;

  for (;;) {
    if (err == 0)
      co_return expected_void;
    if (err != EINPROGRESS) {
      reset(fd);
      co_return Errno{err};
    }

    co_await write_wait(fd);
    write_clear(fd);

    len = sizeof(err);
    if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len) == -1)
      Errno{}.raise("SO_ERROR");
    if (!err) {
      sockaddr_storage peer;
      len = sizeof(peer);
      if (getpeername(fd.get(), reinterpret_cast<sockaddr *>(&peer), &len) ==
          -1) {
        if (errno != ENOTCONN)
          Errno{}.raise("getpeername({}): after SO_ERROR 0", fd.get());
        err = EINPROGRESS;
      }
    }
  }
}

Async<std::expected<int, Errno>>
CoroPoll::tcpconnect(const sockaddr *sa, socklen_t len SLOC_ARG)
{
  if (stop_requested())
    co_return Errno{EBADF};
  UniqueFd fd{socket(sa->sa_family, SOCK_STREAM, 0)};
  if (!fd)
    co_return Errno{};
  auto res = co_await connect(fd, sa, len X23_IFTRACE(, SLOC_VAR));
  if (res)
    co_return fd.release();
  reset(fd);
  co_return res.error();
}

Async<std::expected<int, std::system_error>>
CoroPoll::tcpconnect(std::string host, std::string port SLOC_ARG)
{
  if (stop_requested())
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

} // namespace x23
