#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <system_error>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <gtest/gtest.h>

#include "x23/sockio.h"

using namespace x23;

namespace {

sockaddr_storage
numeric_addr(const char *host, uint16_t port)
{
  sockaddr_storage result{};
  if (strchr(host, ':')) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    EXPECT_EQ(inet_pton(AF_INET6, host, &addr.sin6_addr), 1);
    memcpy(&result, &addr, sizeof(addr));
  }
  else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    EXPECT_EQ(inet_pton(AF_INET, host, &addr.sin_addr), 1);
    memcpy(&result, &addr, sizeof(addr));
  }
  return result;
}

} // namespace

TEST(HostPort, ParseAndFormat)
{
  HostPort hp;
  ASSERT_TRUE(hp.parse("127.0.0.1:1234"));
  EXPECT_EQ(std::string_view(hp.host), "127.0.0.1");
  EXPECT_EQ(std::string_view(hp.port), "1234");
  EXPECT_EQ(std::string_view(hp.show()), "127.0.0.1:1234");
  EXPECT_EQ(std::format("{}", hp), "127.0.0.1:1234");

  ASSERT_TRUE(hp.parse("[2001:db8::1]:65535"));
  EXPECT_EQ(std::string_view(hp.host), "2001:db8::1");
  EXPECT_EQ(std::string_view(hp.port), "65535");
  EXPECT_EQ(std::string_view(hp.show()), "[2001:db8::1]:65535");

  ASSERT_TRUE(hp.parse("example.test:http"));
  EXPECT_EQ(std::string_view(hp.host), "example.test");
  EXPECT_EQ(std::string_view(hp.port), "http");
  for (auto input : {"", "example.test", ":80"}) {
    EXPECT_FALSE(hp.parse(input));
    EXPECT_EQ(std::string_view(hp.host), "example.test");
    EXPECT_EQ(std::string_view(hp.port), "http");
  }
}

TEST(SocketAddress, NumericRoundTrip)
{
  for (auto host : {"192.0.2.1", "2001:db8::1", "::1"}) {
    SCOPED_TRACE(host);
    auto addr = numeric_addr(host, 4321);
    HostPort hp(addr);
    EXPECT_EQ(std::string_view(hp.host), host);
    EXPECT_EQ(std::string_view(hp.port), "4321");
    EXPECT_EQ(get_port(addr), 4321);
    EXPECT_EQ(std::string_view(show_ipaddr(addr)), host);
    EXPECT_EQ(std::string_view(show_addr(addr)), std::string_view(hp.show()));
    EXPECT_EQ(sockaddr_len(addr), addr.ss_family == AF_INET
                                      ? sizeof(sockaddr_in)
                                      : sizeof(sockaddr_in6));
  }

  auto mapped = numeric_addr("::ffff:192.0.2.1", 80);
  EXPECT_EQ(std::string_view(show_ipaddr(mapped)), "192.0.2.1");
  EXPECT_EQ(std::string_view(show_addr(mapped)), "192.0.2.1:80");
}

TEST(SocketAddress, LoopbackRanges)
{
  for (auto host : {"127.0.0.1", "127.42.0.99", "::1", "::ffff:127.0.0.1",
                    "::ffff:127.255.255.254"}) {
    SCOPED_TRACE(host);
    EXPECT_TRUE(is_loopback(numeric_addr(host, 0)));
  }
  for (auto host : {"0.0.0.0", "126.255.255.255", "128.0.0.1", "192.0.2.1",
                    "::", "2001:db8::1", "::ffff:192.0.2.1"}) {
    SCOPED_TRACE(host);
    EXPECT_FALSE(is_loopback(numeric_addr(host, 0)));
  }
}

TEST(SocketAddress, OtherFamilies)
{
  sockaddr_storage addr{};
  addr.ss_family = AF_UNIX;
  EXPECT_EQ(sockaddr_len(addr), sizeof(sockaddr_un));
  EXPECT_EQ(std::string_view(show_addr(addr)), "AF_UNIX");
  EXPECT_FALSE(is_loopback(addr));
  EXPECT_THROW(get_port(addr), std::system_error);
  EXPECT_THROW(show_ipaddr(addr), std::system_error);

  addr.ss_family = AF_UNSPEC;
  EXPECT_EQ(sockaddr_len(addr), socklen_t{0});
  EXPECT_EQ(std::string_view(show_addr(addr)), "sa_family 0");
  EXPECT_FALSE(is_loopback(addr));
  EXPECT_THROW(get_port(addr), std::system_error);
  EXPECT_THROW(show_ipaddr(addr), std::system_error);
}

TEST(SocketOptions, NonblockPreservesFlags)
{
  UniqueFd fd(socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(fd);
  int flags = fcntl(fd.get(), F_GETFL);
  ASSERT_NE(flags, -1);
  for (bool on : {false, true, true, false, false}) {
    set_nonblock(fd, on);
    EXPECT_EQ(fcntl(fd.get(), F_GETFL), on ? flags | O_NONBLOCK : flags);
  }
}

TEST(SocketOptions, NoDelayAndAbortiveClose)
{
  UniqueFd fd(socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(fd);
  for (bool on : {true, false}) {
    set_nodelay(fd, on);
    int value = -1;
    socklen_t len = sizeof(value);
    ASSERT_EQ(getsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &value, &len), 0);
    // Enabled boolean socket options need not read back as exactly 1.
    EXPECT_EQ(value != 0, on);
  }
  enable_abortive_close(fd);
  linger value{};
  socklen_t len = sizeof(value);
  ASSERT_EQ(getsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &value, &len), 0);
  EXPECT_NE(value.l_onoff, 0);
  EXPECT_EQ(value.l_linger, 0);
}

TEST(TcpListen, AddressInUse)
{
  UniqueFd first = stcplisten(numeric_addr("127.0.0.1", 0));
  const sockaddr_storage taken = local_addr(first);

  auto second = try_tcplisten(taken);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error(), EADDRINUSE);

  try {
    stcplisten(taken);
    ADD_FAILURE() << "stcplisten bound an address that is in use";
  } catch (const std::system_error &e) {
    EXPECT_EQ(e.code(), std::errc::address_in_use);
    EXPECT_TRUE(std::string_view(e.what()).starts_with(
        std::format("listen on {}", HostPort(taken))))
        << e.what();
  }
}

TEST(SocketOptions, InvalidDescriptor)
{
  BorrowedFd fd(-1);
  EXPECT_THROW(set_nonblock(fd), std::system_error);
  EXPECT_THROW(set_nodelay(fd), std::system_error);
  EXPECT_THROW(enable_abortive_close(fd), std::system_error);
  EXPECT_THROW(local_addr(fd), std::system_error);
  EXPECT_THROW(remote_addr(fd), std::system_error);
}
