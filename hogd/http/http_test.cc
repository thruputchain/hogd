#include "hogd/http/http.h"

#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "hogd/base/types.h"
#include "hogd/health/health.h"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "gtest/gtest.h"

namespace hogd {
namespace {

std::string
read_all(x23::BorrowedFd fd)
{
  std::string out;
  char buf[1024];
  while (true) {
    ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n <= 0) {
      return out;
    }
    out.append(buf, n);
  }
}

std::string
request(uint16_t port, std::string_view wire)
{
  x23::UniqueFd client = x23::tcpconnect("127.0.0.1", port);
  EXPECT_TRUE(client);
  EXPECT_EQ(::send(client.get(), wire.data(), wire.size(), 0),
            static_cast<ssize_t>(wire.size()));
  return read_all(client);
}

class HttpTest : public ::testing::Test {
protected:
  HttpServer server_;

  uint16_t start()
  {
    return server_.start_server(x23::HostPort{"127.0.0.1", 0});
  }
};

TEST_F(HttpTest, KnownGetRouteReturnsHandlerResponse)
{
  server_.add_route("/hello", [] {
    return HttpResponse{.status = 200,
                        .content_type = "application/test",
                        .body = "hello"};
  });

  std::string response =
      request(start(), "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(response.find("Content-Type: application/test\r\n"),
            std::string::npos);
  EXPECT_NE(response.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_TRUE(response.ends_with("\r\n\r\nhello"));
}

TEST_F(HttpTest, UnknownGetRouteReturns404)
{
  std::string response =
      request(start(), "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
}

TEST_F(HttpTest, UnsupportedMethodReturns405)
{
  server_.add_route("/health", [] {
    return HttpResponse{.status = 200,
                        .content_type = "text/plain",
                        .body = ""};
  });

  std::string response =
      request(start(), "POST /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed\r\n"),
            std::string::npos);
}

TEST_F(HttpTest, MalformedRequestLineReturns400)
{
  std::string response = request(start(), "GET\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
}

TEST_F(HttpTest, MultipleRoutesDispatchIndependently)
{
  server_.add_route("/a", [] {
    return HttpResponse{.status = 200,
                        .content_type = "text/plain",
                        .body = "a"};
  });
  server_.add_route("/b", [] {
    return HttpResponse{.status = 201,
                        .content_type = "text/plain",
                        .body = "b"};
  });

  std::string a =
      request(start(), "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(a.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_TRUE(a.ends_with("\r\n\r\na"));

  std::string b =
      request(server_.port(), "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(b.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
  EXPECT_TRUE(b.ends_with("\r\n\r\nb"));
}

TEST_F(HttpTest, HealthMetricsRoutesReturnExpectedResponses)
{
  add_health_metrics_routes(server_);
  uint16_t port = start();

  std::string health =
      request(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(health.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(health.find("Content-Type: application/json\r\n"),
            std::string::npos);
  EXPECT_NE(health.find("\"process_name\""), std::string::npos);
  EXPECT_NE(health.find("\"status\""), std::string::npos);

  std::string metrics =
      request(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(metrics.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(metrics.find("Content-Type: text/plain\r\n"), std::string::npos);
  EXPECT_NE(metrics.find("Content-Length: 0\r\n"), std::string::npos);
  EXPECT_TRUE(metrics.ends_with("\r\n\r\n"));
}

TEST(HttpServerLifetimeTest, DestructorClosesPartialRequestConnections)
{
  std::promise<uint16_t> port_ready;
  std::promise<void> destroy_requested;
  std::future<void> destroy_requested_future = destroy_requested.get_future();
  std::future<void> server_done =
      std::async(std::launch::async, [&port_ready, &destroy_requested_future] {
        HttpServer server;
        server.add_route("/health", [] {
          return HttpResponse{.status = 200,
                              .content_type = "text/plain",
                              .body = "ok"};
        });
        port_ready.set_value(
            server.start_server(x23::HostPort{"127.0.0.1", 0}));
        destroy_requested_future.wait();
      });

  x23::UniqueFd client =
      x23::tcpconnect("127.0.0.1", port_ready.get_future().get());
  ASSERT_TRUE(client);
  std::string_view partial_request = "GET /health HTTP/1.1\r\nHost:";
  ASSERT_EQ(::send(client.get(), partial_request.data(), partial_request.size(),
                   0),
            static_cast<ssize_t>(partial_request.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  destroy_requested.set_value();
  if (server_done.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    client.reset();
    FAIL() << "HttpServer destructor did not close a partial request "
              "connection";
  }
}

} // namespace
} // namespace hogd
