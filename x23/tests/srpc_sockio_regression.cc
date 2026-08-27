#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <utility>
#include <x23/sockio.h>
#include <x23/srpc.h>

#include "tests/xdrtest.hh"
#include "x23/cleanup.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/rpc_msg.hh"
#include "x23/threadpool.h"
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

// hog-event unclean restart rebinds the EventCard listen ports. The first
// process typically bound port 0; Linux TIME_WAIT then rejects the replacement
// bind unless that original listener had SO_REUSEADDR.
TEST(SockioRegression, EphemeralIpv4ListenRebindsAfterConnectedClose)
{
  auto bind_addr = HostPort{"127.0.0.1", 0}.to_addr();
  ASSERT_TRUE(bind_addr.has_value());
  UniqueFd listener = stcplisten(*bind_addr);
  ASSERT_TRUE(listener);
  const std::uint16_t port = get_port(local_addr(listener));
  ASSERT_NE(port, 0);

  UniqueFd client = tcpconnect("127.0.0.1", port);
  ASSERT_TRUE(client);
  UniqueFd accepted(::accept(listener.get(), nullptr, nullptr));
  ASSERT_TRUE(accepted);

  listener.reset();
  accepted.reset();
  client.reset();

  auto rebound_addr = HostPort{"127.0.0.1", port}.to_addr();
  ASSERT_TRUE(rebound_addr.has_value());
  UniqueFd rebound = stcplisten(*rebound_addr);
  ASSERT_TRUE(rebound);
  EXPECT_EQ(get_port(local_addr(rebound)), port);
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

namespace {

// Metadata for a synthetic non-void RPC whose result is a bounded vector
// (bounded_intvec, max 8 elements).  A malicious peer answers a pending call
// with a SUCCESS reply that declares more elements than the bound permits.
struct oversized_bounded_vec_proc {
  struct version_meta {
    static constexpr std::uint32_t program = 0x5a23'2283;
    static constexpr std::uint32_t version = 1;
    static constexpr const char *version_name = "poc_srpc_v1";
  };

  static constexpr std::uint32_t proc = 1;
  static constexpr const char *proc_name = "oversized_bounded_vec";
  using return_type = bounded_intvec;
};

// Best-effort blocking helpers for the peer side of a loopback socket.
bool
read_exact(int fd, void *buf, size_t len)
{
  auto *p = static_cast<std::uint8_t *>(buf);
  while (len) {
    ssize_t n = ::read(fd, p, len);
    if (n <= 0)
      return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool
write_all(int fd, BufView bytes)
{
  while (!bytes.empty()) {
    ssize_t n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    bytes = bytes.subspan(static_cast<size_t>(n));
  }
  return true;
}

} // namespace

// Regression test for issue #3091: a SUCCESS reply whose bounded-vector result
// declares more elements than the type permits throws XdrLengthError during
// decode.  The SRPC client callback must catch the XdrException base (not just
// XdrBadInput) so the exception does not escape SRpcFd::input(BufView) noexcept
// and call std::terminate.  Expected behaviour: the blocked caller is released
// with a translated XdrBadInput and the process survives.
TEST(SrpcRegression, OversizedBoundedVecReplyIsDecodeError)
{
  auto bind_addr = HostPort{"127.0.0.1", 0}.to_addr();
  ASSERT_TRUE(bind_addr.has_value());
  UniqueFd listener = stcplisten(*bind_addr);
  ASSERT_TRUE(listener);
  uint16_t port = get_port(local_addr(listener));

  UniqueFd client_fd = tcpconnect("127.0.0.1", port);
  ASSERT_TRUE(client_fd);
  UniqueFd peer_fd{::accept(listener.get(), nullptr, nullptr)};
  ASSERT_TRUE(peer_fd);

  RefPtr<SRpcStream<>> client_stream{*new SRpcStream<>(std::move(client_fd))};
  client_stream->go();

  std::atomic<bool> got_xdr_error{false};
  std::thread caller{[&] {
    try {
      (void)client_stream->template send_call<oversized_bounded_vec_proc>();
    } catch (const XdrException &) {
      got_xdr_error = true;
    } catch (...) {
      // Any other outcome (including no exception) leaves got_xdr_error false.
    }
  }};

  // Act as the malicious peer: read the framed request, recover the assigned
  // xid, and answer with a SUCCESS reply whose bounded_intvec count is 9 (the
  // maximum is 8) followed by 8 elements.
  std::uint32_t marker = 0;
  ASSERT_TRUE(read_exact(peer_fd.get(), &marker, sizeof(marker)));
  size_t msglen = getbe<std::uint32_t>(&marker) & 0x7fff'ffffU;
  Buf request;
  request.resize_uninitialized(msglen);
  ASSERT_TRUE(read_exact(peer_fd.get(), request.data(), request.size()));

  rpc_msg reply{};
  reply.xid = getbe<std::uint32_t>(request.data());
  msg_set_reply_stat(&reply, accept_stat::SUCCESS);
  Buf response = make_record(reply, std::uint32_t{9}, std::int32_t{0},
                             std::int32_t{1}, std::int32_t{2}, std::int32_t{3},
                             std::int32_t{4}, std::int32_t{5}, std::int32_t{6},
                             std::int32_t{7}, std::int32_t{8});
  ASSERT_TRUE(write_all(peer_fd.get(), response));

  caller.join();
  EXPECT_TRUE(got_xdr_error.load())
      << "oversized bounded-vector reply should surface as a client-side "
         "XdrException without terminating the process";
}

TEST(RpcRegression, InFlightDisconnectIsTransportException)
{
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  UniqueFd peer_fd{fds[1]};
  ThreadPool thread_pool(2);
  CoroPoll poll(thread_pool);
  RefPtr<RpcStream<>> client_stream{
      *new RpcStream<>(poll, UniqueFd{fds[0]}, RpcNoLogger{})};
  auto client = client_stream->client(blank_rpc_v1{});

  std::atomic<bool> request_read{false};
  std::thread peer{[fd = std::move(peer_fd), &request_read]() mutable {
    std::uint32_t marker = 0;
    if (!read_exact(fd.get(), &marker, sizeof(marker))) {
      return;
    }
    Buf request;
    request.resize_uninitialized(getbe<std::uint32_t>(&marker) & 0x7fff'ffffU);
    if (read_exact(fd.get(), request.data(), request.size())) {
      request_read = true;
    }
  }};

  bool got_transport_error = false;
  try {
    client.null()();
  } catch (const XdrBadInput &e) {
    ADD_FAILURE() << "disconnect was misclassified as malformed XDR: "
                  << e.what();
  } catch (const XdrCallFailed &e) {
    ADD_FAILURE() << "disconnect was misclassified as an RPC rejection: "
                  << e.what();
  } catch (const XdrException &e) {
    got_transport_error = true;
  }
  peer.join();

  EXPECT_TRUE(request_read.load());
  EXPECT_TRUE(got_transport_error);
}

} // namespace x23
