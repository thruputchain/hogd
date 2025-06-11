
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

//#include <print>

namespace x23 {

namespace detail {

ssize_t
ReadBuffer::operator()(int fd, void *buf, size_t len)
{
  if (len == 0)
    return 0;
  assert(len <= PTRDIFF_MAX);

  if (size_t buflen = stop_ - start_) {
    size_t n = std::min(len, buflen);
    memcpy(buf, m_.data() + start_, n);
    start_ += n;
    return n;
  }

  start_ = stop_ = 0;

  struct iovec iov[2];
  iov[0].iov_base = buf;
  iov[0].iov_len = len;
  iov[1].iov_base = m_.data();
  iov[1].iov_len = m_.size();
  auto n = ::readv(fd, iov, 2);
  if (n <= ptrdiff_t(len))
    return n;
  stop_ = n - len;
  return len;
}

Async<std::expected<void,Errno>>
WriteBuffer::write(CoroPoll &cp, BorrowedFd fd, Buf buf)
{
  CoroGuard g;
  co_await g(write_lock_);
  if (co_await writer_.write_or_append(fd.get(), std::move(buf)))
    service(cp, fd);
  co_return expected_void;
}

Detached
WriteBuffer::service(CoroPoll &cp, BorrowedFd fd)
{
  //std::println("Write buffering active on fd {}", fd.get());
  for (;;) {
    CoroGuard g;
    co_await g(write_lock_);
    cp.write_clear(fd);
    auto res = writer_.write(fd);
    if (!res.value_or(false)) {
      //std::println("Write buffering done on fd {}", fd.get());
      co_return;
    }
    g.reset();
    co_await cp.write_wait(fd);
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
      if (Errno err = co_await read_wait(fd.get()))
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

Async<std::expected<void, Errno>>
CoroPoll::bgwrite(BorrowedFd fd, Buf data SLOC_ARG)
{
  auto w = get_writer(fd);
  if (!w)
    co_return Errno{EBADF};
  co_return co_await w->write(*this, fd, std::move(data));
}

Async<std::expected<int, Errno>>
CoroPoll::tcpconnect(const sockaddr *sa, socklen_t len SLOC_ARG)
{
  if (stop_requested())
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
  set_nodelay(fd);
  co_return fd.release();
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
