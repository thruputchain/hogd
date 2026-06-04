#include "hogd/httpd/httpd.h"

#include <string>
#include <string_view>
#include <sys/socket.h>

#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "gtest/gtest.h"

namespace hogd::httpd {
namespace {

std::string
read_all(x23::BorrowedFd fd)
{
  std::string out;
  char buf[1024];
  for (;;) {
    ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n <= 0)
      return out;
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

class HttpdTest : public ::testing::Test {
protected:
  HttpServer server_;

  uint16_t start()
  {
    return server_.start_server(x23::HostPort{"127.0.0.1", 0});
  }
};

TEST_F(HttpdTest, KnownGetRouteReturnsHandlerResponse)
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

TEST_F(HttpdTest, UnknownGetRouteReturns404)
{
  std::string response =
      request(start(), "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
}

TEST_F(HttpdTest, UnsupportedMethodReturns405)
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

TEST_F(HttpdTest, MalformedRequestLineReturns400)
{
  std::string response = request(start(), "GET\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
}

TEST_F(HttpdTest, MultipleRoutesDispatchIndependently)
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

} // namespace
} // namespace hogd::httpd
