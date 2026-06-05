#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/mvar.h"
#include "x23/refcnt.h"
#include "x23/sockio.h"

namespace hogd::httpd {

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain";
  std::string body;
};

class HttpServer : public x23::DestroyInhibit {
public:
  using Handler = std::function<x23::Async<HttpResponse>()>;

  explicit HttpServer(x23::CoroPoll &cp = default_coropoll()) : cp_(cp) {}
  ~HttpServer();

  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;
  HttpServer(HttpServer &&) = delete;
  HttpServer &operator=(HttpServer &&) = delete;

  template<typename F> void add_route(std::string path, F handler)
  {
    add_async_route(std::move(path),
                    [handler = std::move(
                         handler)]() mutable -> x23::Async<HttpResponse> {
                      using Result = std::invoke_result_t<F &>;
                      if constexpr (std::same_as<Result, HttpResponse>) {
                        co_return std::invoke(handler);
                      }
                      else {
                        co_return co_await std::invoke(handler);
                      }
                    });
  }
  uint16_t start_server(x23::HostPort bind_addr);
  uint16_t port() const noexcept { return port_; }

private:
  x23::CoroPoll &cp_;
  x23::MVar<std::map<std::string, Handler>> routes_;
  std::mutex connection_fds_mutex_;
  std::set<int> connection_fds_;
  std::atomic_int listen_fd_{x23::kInvalidFd};
  uint16_t port_{};

  struct ResetFdOnExit;
  struct EofOnExit;
  struct ConnectionFdGuard;

  static x23::Detached accept_loop(x23::CoroPoll &cp, x23::UniqueFd lfd,
                                   x23::RefPtr<HttpServer> self);
  static x23::Detached handle_connection_detached(x23::RefPtr<HttpServer> self,
                                                  x23::UniqueFd fd);
  x23::Async<void> handle_connection(x23::UniqueFd fd);
  x23::Async<HttpResponse> route(std::string_view method,
                                 std::string_view target);
  void add_async_route(std::string path, Handler handler);
  void poison_connection_fds();
  void eof() noexcept;
};

void add_health_metrics_routes(HttpServer &server);

} // namespace hogd::httpd
