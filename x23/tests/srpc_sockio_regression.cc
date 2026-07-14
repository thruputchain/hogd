#include <gtest/gtest.h>

#include <expected>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

#include <utility>
#include <x23/sockio.h>
#include <x23/srpc.h>

#include "tests/xdrtest.hh"
#include "x23/cleanup.h"
#include "x23/refcnt.h"
#include "x23/rpc_msg.hh"
#include "x23/utility.h"
#include "x23/vec.h"

namespace x23 {

TEST(SrpcRegression, ProgramUnavailableReplyIsFramed)
{
  UniqueFd listener = stcplisten(0);
  ASSERT_TRUE(listener);
  uint16_t port = get_port(local_addr(listener));

  UniqueFd client = tcpconnect("127.0.0.1", port);
  ASSERT_TRUE(client);

  UniqueFd accepted(::accept(listener.get(), nullptr, nullptr));
  ASSERT_TRUE(accepted);

  RefPtr<SRpcStream<>> server_stream{*new SRpcStream<>(std::move(accepted))};
  RefPtr<SRpcStream<>> client_stream{*new SRpcStream<>(std::move(client))};
  server_stream->go(); // Intentionally no registered service.
  client_stream->go();

  auto c = client_stream->client(blank_rpc_v1{});
  try {
    c.null();
    FAIL() << "Expected XdrCallFailed(PROG_UNAVAIL)";
  } catch (const XdrCallFailed &e) {
    using enum accept_stat;
    ASSERT_EQ(e.m_.body.mtype(), msg_type::REPLY);
    ASSERT_EQ(e.m_.body.rbody().stat(), reply_stat::MSG_ACCEPTED);
    EXPECT_EQ(e.m_.body.rbody().areply().reply_data.stat(), PROG_UNAVAIL);
  }
}

TEST(SockioRegression, HostPortParseBracketedIpv6)
{
  HostPort hp;
  ASSERT_TRUE(hp.parse("[::1]:443"));
  EXPECT_EQ(std::string_view(hp.host), "::1");
  EXPECT_EQ(std::string_view(hp.port), "443");
}

TEST(SockioRegression, ExplicitIpv4BindReportsActualPort)
{
  auto bind_addr = HostPort{"127.0.0.1", 0}.to_addr();
  ASSERT_TRUE(bind_addr.has_value());

  UniqueFd listener = stcplisten(*bind_addr);
  ASSERT_TRUE(listener);

  sockaddr_storage actual_addr = local_addr(listener);
  EXPECT_EQ(actual_addr.ss_family, AF_INET);
  EXPECT_EQ(std::string_view(HostPort(actual_addr).host), "127.0.0.1");
  EXPECT_NE(get_port(actual_addr), 0);
}

TEST(SockioRegression, ExplicitIpv6BindReportsActualPort)
{
  auto bind_addr = HostPort{"::1", 0}.to_addr();
  if (!bind_addr.has_value())
    GTEST_SKIP() << "IPv6 loopback is unavailable";

  UniqueFd listener = stcplisten(*bind_addr);
  ASSERT_TRUE(listener);

  sockaddr_storage actual_addr = local_addr(listener);
  EXPECT_EQ(actual_addr.ss_family, AF_INET6);
  EXPECT_EQ(std::string_view(HostPort(actual_addr).host), "::1");
  EXPECT_NE(get_port(actual_addr), 0);
}

TEST(SockioRegression, BoundedStrCStr)
{
  BoundedStr<12> bs = "Hello, world";
  ASSERT_EQ(bs.size(), 12U);
  bs.c_str();
}

TEST(SockioRegression, TcpConnectSockaddrReturnsFdOnSuccess)
{
  UniqueFd listener = stcplisten(0);
  ASSERT_TRUE(listener);
  uint16_t port = get_port(local_addr(listener));

  auto target = HostPort{"127.0.0.1", port}.to_addr();
  ASSERT_TRUE(target.has_value());

  std::expected<UniqueFd, Errno> res = tcpconnect(*target);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(*res);
}

TEST(SockioRegression, TcpConnectSockaddrReturnsErrnoOnRefusal)
{
  // Bind a listener to claim a free port, then close it so nothing is
  // listening.  Connecting to the now-vacant port must fail with an
  // unexpected Errno rather than throwing.
  auto bind_addr = HostPort{"127.0.0.1", 0}.to_addr();
  ASSERT_TRUE(bind_addr.has_value());

  uint16_t port = 0;
  {
    UniqueFd listener = stcplisten(*bind_addr);
    ASSERT_TRUE(listener);
    port = get_port(local_addr(listener));
  }

  auto target = HostPort{"127.0.0.1", port}.to_addr();
  ASSERT_TRUE(target.has_value());

  std::expected<UniqueFd, Errno> res = tcpconnect(*target);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().error_code(), std::errc::connection_refused);
}

} // namespace x23
