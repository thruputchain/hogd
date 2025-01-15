
#include <cerrno>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "x23/async.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/fileio.h"
#include "x23/threadpool.h"

namespace x23 {

CoroPoll::CoroPoll(ThreadPool &pool) : CoroPollImpl(pool)
{
  pollthread_ = std::thread(&CoroPoll::loop, this);
}

CoroPoll::~CoroPoll()
{
  std::printf("start ~coropoll %p\n", this);
  request_stop();
  std::printf("stop requested %p\n", this);
  pollthread_.join();
  destroy_wait();
}

void
CoroPoll::loop()
{
  while (!time_to_stop())
    check();
  std::printf("done Coropoll:loop %p\n", this);
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

Async<std::expected<int, Errno>>
CoroPoll::tcpconnect(sockaddr *sa, socklen_t len)
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
