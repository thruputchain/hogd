#pragma once

#include <chrono>
#include <future>
#include <initializer_list>
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

// Send a request as a sequence of separate TCP segments, pausing between
// each so the server is forced to satisfy it across multiple reads.
inline std::string
request_chunked(uint16_t port, std::initializer_list<std::string_view> chunks)
{
  x23::UniqueFd client = x23::tcpconnect("127.0.0.1", port);
  EXPECT_TRUE(client);
  bool first = true;
  for (std::string_view chunk : chunks) {
    if (!first) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    first = false;
    EXPECT_EQ(::send(client.get(), chunk.data(), chunk.size(), 0),
              static_cast<ssize_t>(chunk.size()));
  }
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
