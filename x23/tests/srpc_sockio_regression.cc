#include <gtest/gtest.h>

#include <string_view>
#include <sys/socket.h>
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

TEST(SockioRegression, BoundedStrCStr)
{
  BoundedStr<12> bs = "Hello, world";
  ASSERT_EQ(bs.size(), 12U);
  bs.c_str();
}

} // namespace x23
