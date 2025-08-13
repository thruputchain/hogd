
#include <cerrno>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "x23/async.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

#include <print>

namespace x23 {

namespace detail {

Errno
WriteBuffer::write(CoroPoll &cp, BorrowedFd fd, Buf buf)
{
  if (service_lock_.try_lock()) {
    std::unique_lock g(service_lock_, std::adopt_lock);
    auto ptr = writer_.take();
    auto res = ptr->write_or_append(fd.get(), std::move(buf));
    if (res && !ptr->empty()) {
      ptr.reset();
      service(cp, fd, std::move(g));
    }
    else {
      if (!res)
	cp.poison(fd);
      g.unlock();
    }
    return res.error_or(0);
  }

  writer_->append(std::move(buf));
  if (service_lock_.try_lock())
    service(cp, fd, {service_lock_, std::adopt_lock});
  return 0;
}

Detached
WriteBuffer::service(CoroPoll &cp, BorrowedFd fd,
                     std::unique_lock<std::mutex> g)
{
  assert(g.owns_lock() && g.mutex() == &service_lock_);

  co_await cp.pool_.bg();
  for (;;) {
    BufVec work;
    if (auto ptr = writer_.take(); work.swap(*ptr), work.empty()) {
      // If we are here, we must guarantee that at least one thread
      // concurrently appending to writer_ will be able to acquire
      // service_lock_.  That means we must release service_lock_
      // before writer_.
      g.unlock();
      co_return;
    }

    for (;;) {
      cp.write_clear(fd);
      auto res = work.write(fd);
      if (!res)	{		// write returned error
	cp.poison(fd);
	co_return;
      }
      if (!*res)		// no data left to write
	break;
      co_await cp.write_wait(fd);
    }
  }
}

} // namespace detail

CoroPoll::CoroPoll(ThreadPool &pool) : CoroPollImpl(pool)
{
  pollthread_ = std::thread(&CoroPoll::loop, this);
  if (ignore_sigpipe) {
    ignore_sigpipe = false;
    signal(SIGPIPE, SIG_IGN);
  }
}

CoroPoll::~CoroPoll()
{
  request_stop();
  for (int i = 0, e = get_max_fd(); i < e; ++i)
    reset(i);
  wake();
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
CoroPoll::read(BorrowedFd fd, Buf *out, size_t len, bool full SLOC_ARG)
{
  size_t startsize = out->size();
  while (len > 0) {
    read_clear(fd);
    auto r = get_reader(fd);
    if (!r)
      co_return EBADF;
    if (auto res = r->read(fd, out, len)) {
      len -= *res;
      if (*res == 0 || !full)
        break;
    }
    else if (res.error() == EAGAIN) {
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
CoroPoll::bgwrite(BorrowedFd fd, Buf data)
{
  auto w = get_writer(fd);
  if (!w)
    return EBADF;
  Errno err = w->write(*this, fd, std::move(data));
  if (err)
    poison(fd);
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
  write_clear(fd);
  if (::connect(fd.get(), sa, len) == -1)
    err = errno;

  for (;;) {
    if (err == 0)
      co_return expected_void;
    if (err != EINPROGRESS)
      co_return Errno{err};

    co_await write_wait(fd);
    write_clear(fd);

    len = sizeof(err);
    if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len) == -1)
      Errno{}.raise("SO_ERROR");
    if (!err) {
      struct sockaddr peer;
      len = sizeof(peer);
      if (getpeername(fd.get(), &peer, &len) == -1) {
        if (errno != ENOTCONN)
          Errno{}.raise("getpeername({}) after SO_ERROR 0", fd.get());
        err = EINPROGRESS;
      }
    }
    else
      std::println("SO_ERROR: fd {}, err {}", fd.get(), Errno{err});
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
