#pragma once

#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "gtest/gtest.h"

namespace hogd {

inline std::string
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

inline std::string
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

} // namespace hogd
