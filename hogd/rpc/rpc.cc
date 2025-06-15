// rpc.cc - a high-performance RPC framework that focuses on adaptive batching
//
// TODO(abelay): create a Socket class as a cleanup.
// TODO(abelay): need to think more about error handling when sockets fail.

#include "hogd/rpc/rpc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <future>
#include <mutex>
#include <type_traits>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {

template<typename Result, typename T>
ErrnoStatus<Result>
make_errno_status(T ret)
{
  // first check for error.
  if (ret == -1)
    return Errno{};

  // otherwise return the result type.
  assert(ret >= 0);
  if constexpr (std::is_integral_v<Result>) {
    return static_cast<Result>(ret);
  }
  else if (std::is_void_v<Result>) {
    assert(ret == 0);
    return {};
  }
  else {
    static_assert("unsupported result type");
  }
}

inline ErrnoStatus<int>
sys_socket(int domain, int type, int protocol)
{
  return make_errno_status<int>(socket(domain, type, protocol));
}

inline ErrnoStatus<void>
sys_connect(int sockfd, const sockaddr_in &addr)
{
  return make_errno_status<void>(
      connect(sockfd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)));
}

inline ErrnoStatus<int>
sys_accept(int sockfd, sockaddr_in *addr)
{
  if (!addr)
    return make_errno_status<int>(accept(sockfd, nullptr, nullptr));
  socklen_t len;
  return make_errno_status<int>(
      accept(sockfd, reinterpret_cast<sockaddr *>(addr), &len));
}

inline ErrnoStatus<void>
sys_bind(int sockfd, const sockaddr_in &addr)
{
  return make_errno_status<void>(
      bind(sockfd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)));
}

inline ErrnoStatus<void>
sys_listen(int sockfd, int backlog)
{
  return make_errno_status<void>(listen(sockfd, backlog));
}

template<typename T> requires std::is_trivially_copyable_v<T>
inline ErrnoStatus<void>
sys_setsockopt(int sockfd, int level, int option_name, const T &value)
{
  return make_errno_status<void>(
      setsockopt(sockfd, level, option_name,
                 reinterpret_cast<const void *>(&value), sizeof(value)));
}

inline ErrnoStatus<void>
sys_shutdown(int sockfd, int how)
{
  return make_errno_status<void>(shutdown(sockfd, how));
}

inline ErrnoStatus<size_t>
read_size(BufferedReader &r)
{
  size_t size;
  ErrnoStatus<void> ret = read_full(r, writable_byte_view(size));
  if (!ret)
    return ret.error();
  return size;
}

inline ErrnoStatus<OwningSpan<const std::byte>>
read_bytes(BufferedReader &r, size_t n)
{
  auto buf = std::make_unique_for_overwrite<std::byte[]>(n);
  ErrnoStatus<void> ret = read_full(r, std::span<std::byte>(buf.get(), n));
  if (!ret)
    return ret.error();
  auto *base_ptr = buf.get();
  return OwningSpan(std::span<const std::byte>(base_ptr, n),
                    [ref = std::move(buf)] {});
}

} // namespace

RPCClient::~RPCClient()
{
  if (ErrnoStatus<void> ret = sys_shutdown(socket_.get_fd(), SHUT_RDWR); !ret)
    error("could not shutdown socket: {}", ret.error());
}

ErrnoStatus<std::unique_ptr<RPCClient>>
RPCClient::dial(std::string_view addr)
{
  std::vector<std::string_view> args = split(addr, ':');
  if (args.size() != 2)
    return Errno(EINVAL);
  std::optional<uint16_t> port = parse_integer<uint16_t>(args[1]);
  if (!port)
    return Errno(EINVAL);

  ErrnoStatus<int> fd = sys_socket(AF_INET, SOCK_STREAM, 0);
  if (!fd)
    return fd.error();

  sockaddr_in saddr;
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = inet_addr(std::string(args[0]).c_str());
  saddr.sin_port = htons(*port);
  ErrnoStatus<void> ret = sys_connect(*fd, saddr);
  if (!ret)
    return ret.error();

  ret = sys_setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, int(1));
  if (!ret)
    return ret.error();

  info("rcp client connected to {}", addr);
  return std::make_unique<RPCClient>(File(*fd));
}

std::future<OwningSpan<const std::byte>>
RPCClient::call(std::span<const std::byte> args)
{
  std::promise<OwningSpan<const std::byte>> promise;
  std::future<OwningSpan<const std::byte>> future = promise.get_future();

  std::scoped_lock g(lock_);
  requests_.push(args);
  completions_.emplace(std::move(promise));
  request_cv_.notify_one();
  return future;
}

void
RPCClient::send_worker(const std::stop_token &stoken)
{
  BufferedWriter w(socket_);
  while (!stoken.stop_requested()) {
    std::queue<Request> requests;
    {
      std::unique_lock ul(lock_);
      request_cv_.wait(ul, stoken, [this] { return !requests_.empty(); });
      if (stoken.stop_requested()) [[unlikely]]
        return;
      std::swap(requests_, requests);
    }

    while (!requests.empty()) {
      Request req = requests.front();
      requests.pop();
      size_t req_bytes = req.size();
      if (ErrnoStatus<void> ret = write_full(w, byte_view(req_bytes)); !ret) {
        error("client write size failed: {}", ret.error());
        return;
      }
      if (ErrnoStatus<void> ret = write_full(w, req); !ret) {
        error("client write bytes failed: {}", ret.error());
        return;
      }
    }

    w.flush();
  }
}

void
RPCClient::receive_worker(const std::stop_token &stoken)
{
  std::queue<Completion> completions;
  auto take_completion = [this, &completions] {
    if (completions.empty()) {
      std::scoped_lock g(lock_);
      std::swap(completions_, completions);
    }

    assert(!completions.empty());
    Completion c = std::move(completions.front());
    completions.pop();
    return c;
  };

  BufferedReader r(socket_);
  while (!stoken.stop_requested()) {
    ErrnoStatus<size_t> n = read_size(r);
    if (!n) {
      if (n.error() != ENODATA)
        warn("client read size failed: {}", n.error());
      return;
    }
    ErrnoStatus<OwningSpan<const std::byte>> bytes = read_bytes(r, *n);
    if (!bytes) {
      if (bytes.error() != ENODATA)
        warn("client read bytes failed: {}", bytes.error());
      return;
    }
    take_completion().set_value(std::move(*bytes));
  }
}

ErrnoStatus<void>
RPCServer::listen()
{
  ErrnoStatus<int> accept_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
  if (!accept_fd)
    return accept_fd.error();

  ErrnoStatus<void> ret =
      sys_setsockopt(*accept_fd, SOL_SOCKET, SO_REUSEPORT, int(1));
  if (!ret)
    return ret.error();

  sockaddr_in saddr;
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  saddr.sin_port = htons(port_);
  ret = sys_bind(*accept_fd, saddr);
  if (!ret)
    return ret.error();

  ret = sys_listen(*accept_fd, 10);
  if (!ret)
    return ret.error();

  while (true) {
    ErrnoStatus<int> fd = sys_accept(*accept_fd, nullptr);
    if (!fd)
      return fd.error();
    ErrnoStatus<void> ret =
        sys_setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, int(1));
    if (!ret)
      return ret.error();
    std::jthread([this, fd = *fd] { connection_worker(File(fd)); }).detach();
  }

  return {};
}

void
RPCServer::connection_worker(File &&socket)
{
  BufferedReader r(socket);
  BufferedWriter w(socket);
  std::vector<OwningSpan<const std::byte>> requests;
  std::optional<size_t> n;

  while (true) {
    // read available requests.
    // GOAL: fetch as many requests as possible without blocking by peaking at
    // how many are currently buffered.
    size_t next_n = 0;
    while (r.size() >= next_n || requests.empty()) {
      if (!n) {
        ErrnoStatus<size_t> size = read_size(r);
        if (!size) {
          if (size.error() != ENODATA)
            error("server read size failed: {}", size.error());
          return;
        }
        n = next_n = *size;
        continue;
      }

      ErrnoStatus<OwningSpan<const std::byte>> bytes = read_bytes(r, *n);
      if (!bytes) {
        if (bytes.error() != ENODATA)
          error("server read bytes failed: {}", bytes.error());
        return;
      }
      requests.emplace_back(std::move(*bytes));
      next_n = sizeof(size_t);
      n.reset();
    }

    // execute available requests.
    for (const auto &req : requests) {
      OwningSpan<const std::byte> bytes = handler_(req);
      size_t resp_bytes = bytes.size();
      if (ErrnoStatus<void> ret = write_full(w, byte_view(resp_bytes)); !ret) {
        error("server write size failed: {}", ret.error());
        return;
      }
      if (ErrnoStatus<void> ret = write_full(w, bytes); !ret) {
        error("server write bytes failed: {}", ret.error());
        return;
      }
    }

    // flush anything left in the response buffer.
    w.flush();
    requests.clear();
  }
}

} // namespace hogd
