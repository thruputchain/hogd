#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include <x23/rpc_common.h>
#include <x23/sockio.h>
#include <x23/srpc.h>

namespace x23 {
namespace {

struct DummyVersionMeta {
  static constexpr std::uint32_t program = 0x7f00aa11;
  static constexpr std::uint32_t version = 1;
};

struct DummyProcMeta {
  using version_meta = DummyVersionMeta;
  static constexpr std::uint32_t proc = 1;
};

} // namespace

TEST(SrpcRegression, ProgramUnavailableReplyIsFramed)
{
  UniqueFd listener = stcplisten(0);
  ASSERT_TRUE(listener);
  uint16_t port = get_port(local_addr(listener));

  UniqueFd client = tcpconnect("127.0.0.1", port);
  ASSERT_TRUE(client);

  UniqueFd accepted(::accept(listener.get(), nullptr, nullptr));
  ASSERT_TRUE(accepted);

  RefPtr<SRpcStream<>> server{*new SRpcStream<>(std::move(accepted))};
  server->go();

  rpc_msg call = call_msg<DummyProcMeta>();
  call.xid = 0x01020304;
  Buf request = make_record(call);

  for (size_t sent = 0; sent < request.size();) {
    ssize_t n = ::write(client.get(), request.data() + sent, request.size() - sent);
    ASSERT_GT(n, 0);
    sent += static_cast<size_t>(n);
  }

  std::array<std::uint8_t, 4> header{};
  for (size_t got = 0; got < header.size();) {
    ssize_t n = ::read(client.get(), header.data() + got, header.size() - got);
    ASSERT_GT(n, 0);
    got += static_cast<size_t>(n);
  }

  const std::uint32_t first_word = getbe<std::uint32_t>(header.data());
  EXPECT_NE(first_word, call.xid);
  EXPECT_NE(first_word & 0x80000000U, 0U);
}

TEST(SockioRegression, HostPortParseBracketedIpv6)
{
  HostPort hp;
  ASSERT_TRUE(hp.parse("[::1]:443"));
  EXPECT_EQ(std::string_view(hp.host), "::1");
  EXPECT_EQ(std::string_view(hp.port), "443");
}

} // namespace x23
