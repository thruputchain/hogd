#include "hogd/httpd/httpd.h"

#include <atomic>
#include <exception>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <tuple>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/health/health.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/refcnt.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd::httpd {
namespace {

constexpr size_t kMaxRequestHeaderBytes = 16 * 1024;

std::string_view
reason_phrase(int status)
{
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 418: // haha
    return "I'm a teapot";
  case 500:
    return "Internal Server Error";
  default:
    return "Unknown";
  }
}

bool
has_complete_headers(std::string_view request)
{
  return request.find("\r\n\r\n") != std::string_view::npos ||
         request.find("\n\n") != std::string_view::npos;
}

std::string_view
first_line(std::string_view request)
{
  size_t end = request.find('\n');
  if (end == std::string_view::npos)
    return request;
  if (end && request[end - 1] == '\r')
    --end;
  return request.substr(0, end);
}

std::optional<std::tuple<std::string_view, std::string_view, std::string_view>>
parse_request_line(std::string_view line)
{
  const size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos)
    return std::nullopt;
  const size_t second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos)
    return std::nullopt;
  if (line.find(' ', second_space + 1) != std::string_view::npos)
    return std::nullopt;

  std::string_view method = line.substr(0, first_space);
  std::string_view target =
      line.substr(first_space + 1, second_space - first_space - 1);
  std::string_view version = line.substr(second_space + 1);
  if (method.empty() || target.empty() ||
      (version != "HTTP/1.1" && version != "HTTP/1.0")) {
    return std::nullopt;
  }
  return std::tuple{method, target, version};
}

std::string_view
path_without_query(std::string_view target)
{
  return target.substr(0, target.find('?'));
}

std::string
serialize_response(const HttpResponse &response)
{
  const std::string_view reason = reason_phrase(response.status);
  return std::format("HTTP/1.1 {} {}\r\n"
                     "Content-Type: {}\r\n"
                     "Content-Length: {}\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "{}",
                     response.status, reason, response.content_type,
                     response.body.size(), response.body);
}

HttpResponse
plain_response(int status, std::string body)
{
  return HttpResponse{.status = status,
                      .content_type = "text/plain",
                      .body = std::move(body)};
}

} // namespace

HttpServer::~HttpServer()
{
  if (int lfd = listen_fd_; lfd != x23::kInvalidFd) {
    cp_.poison(lfd);
    listen_fd_.wait(lfd);
  }
  destroy_wait();
}

void
HttpServer::add_async_route(std::string path, Handler handler)
{
  CHECK(!path.empty() && path.front() == '/');
  CHECK(static_cast<bool>(handler));
  auto routes = routes_.take();
  (*routes)[std::move(path)] = std::move(handler);
}

uint16_t
HttpServer::start_server(x23::HostPort bind_addr)
{
  CHECK_EQ(listen_fd_.load(std::memory_order_relaxed), x23::kInvalidFd);
  std::expected<sockaddr_storage, x23::GaiError> addr =
      bind_addr.to_addr(/*dns=*/false);
  CHECK(addr.has_value());

  int backlog = 5 + cp_.pool_.nthreads_;
  x23::UniqueFd lfd = x23::tcplisten(*addr, backlog);
  port_ = x23::get_port(x23::local_addr(lfd));
  listen_fd_ = lfd.get();

  [&cp = cp_, lfd = std::move(lfd), self = x23::RefPtr(*this),
   _d = x23::Defer([this] { eof(); })](this auto /*copy*/) -> x23::Detached {
    x23::Defer _resetfd{[&lfd, &cp] { cp.reset(lfd); }};
    while (auto c = co_await cp.accept(lfd)) {
      [self, fd = x23::UniqueFd{*c}]() mutable -> x23::Detached {
        co_await self->handle_connection(std::move(fd));
      }();
    }
  }();

  return port_;
}

x23::Async<void>
HttpServer::handle_connection(x23::UniqueFd fd)
{
  x23::Buf buf;
  std::string request;
  while (request.size() < kMaxRequestHeaderBytes &&
         !has_complete_headers(request)) {
    auto read_result = co_await cp_.read(fd, &buf, 4096, /*full=*/false);
    if (!read_result || *read_result == 0) {
      co_return;
    }
    request.append(reinterpret_cast<const char *>(buf.data()), buf.size());
  }

  HttpResponse response;
  std::optional<
      std::tuple<std::string_view, std::string_view, std::string_view>>
      parsed;
  if (has_complete_headers(request)) {
    parsed = parse_request_line(first_line(request));
  }
  if (!parsed.has_value()) {
    response = plain_response(400, "Bad Request\n");
  }
  else {
    auto [method, target, version] = *parsed;
    (void)version;
    response = co_await route(method, target);
  }

  std::string serialized = serialize_response(response);
  (void)co_await cp_.write(fd, x23::BufView(serialized));
}

x23::Async<HttpResponse>
HttpServer::route(std::string_view method, std::string_view target)
{
  if (method != "GET")
    co_return plain_response(405, "Method Not Allowed\n");

  std::string path(path_without_query(target));
  Handler handler;
  auto routes = routes_.take();
  auto it = routes->find(path);
  if (it == routes->end())
    co_return plain_response(404, "Not Found\n");
  handler = it->second;
  routes.reset();

  try {
    co_return co_await handler();
  } catch (const std::exception &e) {
    co_return plain_response(500, std::format("Internal Server Error: {}\n",
                                              e.what()));
  } catch (...) {
    co_return plain_response(500, "Internal Server Error\n");
  }
}

void
HttpServer::eof() noexcept
{
  listen_fd_ = x23::kInvalidFd;
  listen_fd_.notify_all();
}

void
add_health_metrics_routes(HttpServer &server)
{
  server.add_route("/health", []() -> x23::Async<HttpResponse> {
    co_return HttpResponse{
        .status = 200,
        .content_type = "application/json",
        .body = co_await Health::instance().dump_to_string(),
    };
  });
  server.add_route("/metrics", [] {
    return HttpResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = "",
    };
  });
}

} // namespace hogd::httpd
