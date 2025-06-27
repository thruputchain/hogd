// rpc.cc - a high-performance RPC framework that focuses on adaptive batching
//
// TODO(abelay): create a Socket class as a cleanup.

#include "hogd/rpc/rpc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/base/cleanup.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
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
  socklen_t len; // NOLINT(misc-include-cleaner)
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

template<typename T> requires std::is_trivially_copyable_v<T>
inline ErrnoStatus<void>
sys_getsockname(int sockfd, T &val)
{
  socklen_t len = sizeof(val);
  return make_errno_status<void>(
      getsockname(sockfd, reinterpret_cast<sockaddr *>(&val), &len));
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

namespace rpc_detail {

ClientFlow::~ClientFlow()
{
  if (ErrnoStatus<void> ret = sys_shutdown(socket_.get_fd(), SHUT_RDWR); !ret)
    error("could not shutdown socket: {}", ret.error());
}

inline void
ClientFlow::abort_call(Completion &completion)
{
  completion.set_exception(
      std::make_exception_ptr(std::runtime_error("network connection lost")));
}

void
ClientFlow::abort_calls(std::queue<Completion> &&completions)
{
  while (!completions.empty()) {
    abort_call(completions.front());
    completions.pop();
  }
}

void
ClientFlow::fail()
{
  std::scoped_lock g(lock_);
  abort_calls(std::move(completions_));
  connection_failed_ = true;
  request_cv_.notify_one();
}

std::future<OwningSpan<const std::byte>>
ClientFlow::call(std::span<const std::byte> args)
{
  std::promise<OwningSpan<const std::byte>> promise;
  std::future<OwningSpan<const std::byte>> future = promise.get_future();

  std::scoped_lock g(lock_);
  if (connection_failed_) [[unlikely]] {
    abort_call(promise);
    return future;
  }
  requests_.push(args);
  completions_.emplace(std::move(promise));
  request_cv_.notify_one();
  return future;
}

void
ClientFlow::send_worker(const std::stop_token &stoken)
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
      if (connection_failed_) [[unlikely]]
        return;
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

    if (ErrnoStatus<void> ret = w.flush(); !ret) {
      error("client flush failed: {}", ret.error());
      return;
    }
  }
}

void
ClientFlow::receive_worker(const std::stop_token &stoken)
{
  std::queue<Completion> completions;
  // called when the socket fails.
  Defer d([this, &completions] {
    abort_calls(std::move(completions));
    fail();
  });

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

} // namespace rpc_detail

ErrnoStatus<RPCClient>
RPCClient::dial(std::string_view addr, int flows)
{
  std::vector<std::string_view> args = split(addr, ':');
  if (args.size() != 2)
    return Errno(EINVAL);
  std::optional<uint16_t> port = parse_integer<uint16_t>(args[1]);
  if (!port)
    return Errno(EINVAL);

  sockaddr_in saddr;
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = inet_addr(std::string(args[0]).c_str());
  saddr.sin_port = htons(*port);

  std::vector<std::unique_ptr<rpc_detail::ClientFlow>> flow_tbl;
  flow_tbl.reserve(flows);
  for (int i = 0; i < flows; ++i) {
    ErrnoStatus<int> fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    if (!fd)
      return fd.error();

    ErrnoStatus<void> ret = sys_connect(*fd, saddr);
    if (!ret)
      return ret.error();

    ret = sys_setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, 1);
    if (!ret)
      return ret.error();
    flow_tbl.emplace_back(std::make_unique<rpc_detail::ClientFlow>(File(*fd)));
  }

  info("rpc client connected to {} with {} flows", addr, flows);
  return RPCClient(std::move(flow_tbl));
}

ErrnoStatus<std::shared_ptr<RPCServer>>
RPCServer::create(uint16_t port, RPCHandler handler)
{
  ErrnoStatus<int> accept_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
  if (!accept_fd)
    return accept_fd.error();

  ErrnoStatus<void> ret =
      sys_setsockopt(*accept_fd, SOL_SOCKET, SO_REUSEPORT, 1); // NOLINT
  if (!ret)
    return ret.error();

  sockaddr_in saddr;
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  saddr.sin_port = htons(port);
  ret = sys_bind(*accept_fd, saddr);
  if (!ret)
    return ret.error();

  constexpr int kAcceptBacklog = 1024;
  ret = sys_listen(*accept_fd, kAcceptBacklog);
  if (!ret)
    return ret.error();

  return std::make_shared<RPCServer>(std::move(handler), File(*accept_fd));
}

ErrnoStatus<uint16_t>
RPCServer::get_port() const
{
  sockaddr_in addr;
  ErrnoStatus<void> ret = sys_getsockname(listener_.get_fd(), addr);
  if (!ret)
    return ret.error();
  return ntohs(addr.sin_port);
}

ErrnoStatus<void>
RPCServer::listen()
{
  while (true) {
    ErrnoStatus<int> fd = sys_accept(listener_.get_fd(), nullptr);
    if (!fd)
      return fd.error();
    ErrnoStatus<void> ret = sys_setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, 1);
    if (!ret)
      return ret.error();
    std::jthread([this, fd = *fd, h = handler_] {
      RPCServer::connection_worker(shared_from_this(), File(fd), std::move(h));
    }).detach();
  }

  return {};
}

void
RPCServer::connection_worker(std::shared_ptr<RPCServer> ref, File &&socket,
                             const RPCHandler &handler)
{
  BufferedReader r(socket);
  BufferedWriter w(socket);
  std::vector<OwningSpan<const std::byte>> requests;
  std::optional<size_t> n;

  ref->register_worker(socket);
  Defer d([&ref, &socket] { ref->unregister_worker(socket); });

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
      OwningSpan<const std::byte> bytes = handler(req);
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
    if (ErrnoStatus<void> ret = w.flush(); !ret) {
      error("server flush failed: {}", ret.error());
      return;
    }
    requests.clear();
  }
}

void
RPCServer::stop()
{
  std::scoped_lock guard(lock_);
  if (ErrnoStatus<void> ret = sys_shutdown(listener_.get_fd(), SHUT_RD); !ret) {
    error("could not shutdown listener socket: {}", ret.error());
  }

  for (File *f : workers_) {
    if (ErrnoStatus<void> ret = sys_shutdown(f->get_fd(), SHUT_RD); !ret)
      error("could not shutdown worker socket: {}", ret.error());
  }
}

} // namespace hogd
